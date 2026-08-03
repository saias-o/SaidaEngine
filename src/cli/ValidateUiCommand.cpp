#include "cli/ValidateUiCommand.hpp"

#include "cli/UiCommandSupport.hpp"
#include "cli/UiDocumentSession.hpp"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace saida {

namespace {

using json = nlohmann::json;
using namespace saida::ui_cli;

struct Issue {
    std::string kind;
    std::string where;    // element path, or file:line
    std::string message;
};

// ── 1. Whatever the engine and RmlUi complained about while loading ─────────

// Paths in engine diagnostics are absolute because the engine resolved them;
// a report about a project names things the way the project does.
std::string relativizePaths(std::string message, const std::filesystem::path& projectRoot) {
    const std::string root = projectRoot.generic_string();
    if (root.empty()) return message;
    for (size_t at = message.find(root); at != std::string::npos; at = message.find(root, at)) {
        size_t length = root.size();
        if (at + length < message.size() && message[at + length] == '/') ++length;
        message.erase(at, length);
    }
    return message;
}

void collectReportedDiagnostics(const UiDocumentSession& session,
                                const std::filesystem::path& projectRoot,
                                std::vector<Issue>& issues) {
    for (const RmlUiDiagnostic& diagnostic : session.diagnostics()) {
        if (diagnostic.severity == "info") continue;
        // A missing image is an unresolved asset reference, which the roadmap
        // item names separately; keeping the two apart lets a caller filter.
        const bool missingAsset = diagnostic.message.rfind("texture not found:", 0) == 0;
        issues.push_back({missingAsset ? "unresolved-asset" : "rejected-declaration",
                          session.relativePath(),
                          relativizePaths(diagnostic.message, projectRoot)});
    }
}

// ── 2. An element that computes to `display: inline` while carrying box
//       properties. Always a bug: RCSS discards them without a word ──────────

bool isDefinite(Rml::Style::LengthPercentageAuto value) {
    return value.type != Rml::Style::LengthPercentageAuto::Auto;
}

const char* textAlignName(Rml::Style::TextAlign align) {
    switch (align) {
    case Rml::Style::TextAlign::Left: return "left";
    case Rml::Style::TextAlign::Right: return "right";
    case Rml::Style::TextAlign::Center: return "center";
    case Rml::Style::TextAlign::Justify: return "justify";
    }
    return "unknown";
}

void checkInlineBoxProperties(Rml::Element& element, std::vector<Issue>& issues) {
    // RmlUi's anonymous text elements are always inline and are not authored;
    // reporting them would bury the real findings under one entry per string.
    const bool anonymous = !element.GetTagName().empty() && element.GetTagName()[0] == '#';
    if (!anonymous && element.GetComputedValues().display() == Rml::Style::Display::Inline) {
        const Rml::ComputedValues& computed = element.GetComputedValues();
        std::vector<std::string> discarded;
        if (isDefinite(computed.width())) discarded.emplace_back("width");
        if (isDefinite(computed.height())) discarded.emplace_back("height");

        // text-align inherits, so a value matching the parent's was not set
        // here and is doing exactly what its author intended further up.
        Rml::Element* parent = element.GetParentNode();
        if (parent != nullptr &&
            computed.text_align() != parent->GetComputedValues().text_align()) {
            discarded.emplace_back(std::string("text-align: ") +
                                   textAlignName(computed.text_align()));
        }

        if (!discarded.empty()) {
            std::string list;
            for (const std::string& name : discarded) {
                if (!list.empty()) list += ", ";
                list += name;
            }
            issues.push_back({"inline-box-properties", elementPath(element),
                              "computes to display: inline, so " + list +
                                  " is discarded — declare a display (block, inline-block, flex)"});
        }
    }

    for (int i = 0; i < element.GetNumChildren(); ++i) {
        if (Rml::Element* child = element.GetChild(i)) {
            checkInlineBoxProperties(*child, issues);
        }
    }
}

// ── 3. `rgba()` with a 0-1 alpha. RCSS alpha is 0-255, so `rgba(255,0,0,0.5)`
//       parses as nothing and the whole declaration vanishes, unreported ─────

// Read the number starting at `pos`; returns false when there is none.
bool readNumber(const std::string& text, size_t pos, size_t& end, double& value) {
    size_t i = pos;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    const size_t start = i;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
    bool digits = false;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) { ++i; digits = true; }
    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) { ++i; digits = true; }
    }
    if (!digits) return false;
    value = std::stod(text.substr(start, i - start));
    end = i;
    return true;
}

void checkFractionalRgbaAlpha(const std::string& fileLabel, const std::string& text,
                              std::vector<Issue>& issues) {
    size_t search = 0;
    while ((search = text.find("rgba(", search)) != std::string::npos) {
        const size_t open = search + 5;
        const size_t close = text.find(')', open);
        search = open;
        if (close == std::string::npos) continue;

        const std::string arguments = text.substr(open, close - open);
        // The alpha is the fourth component.
        size_t component = 0;
        size_t cursor = 0;
        double alpha = 0.0;
        bool haveAlpha = false;
        while (cursor <= arguments.size()) {
            size_t end = 0;
            double value = 0.0;
            if (!readNumber(arguments, cursor, end, value)) break;
            ++component;
            if (component == 4) {
                alpha = value;
                haveAlpha = true;
                break;
            }
            const size_t comma = arguments.find(',', end);
            if (comma == std::string::npos) break;
            cursor = comma + 1;
        }

        // 0 and 1 are valid 0-255 alphas (both nearly transparent), but a value
        // strictly between them can only have been meant as a CSS 0-1 alpha.
        if (haveAlpha && alpha > 0.0 && alpha < 1.0) {
            const size_t line = 1 + static_cast<size_t>(
                                        std::count(text.begin(), text.begin() + static_cast<long>(search), '\n'));
            issues.push_back({"fractional-rgba-alpha",
                              fileLabel + ":" + std::to_string(line),
                              "rgba(" + arguments + ") has a 0-1 alpha, but RCSS alpha is 0-255: "
                              "this parses as nothing and the whole declaration is dropped "
                              "(use " + std::to_string(static_cast<int>(alpha * 255.0 + 0.5)) +
                              ", or a #RRGGBBAA colour)"});
        }
    }
}

// ── 4. Files the document pulled in but could not open ──────────────────────

bool readFile(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

struct Options {
    std::string document;
    std::string projectRoot;
    uint32_t width = kDefaultWidth;
    uint32_t height = kDefaultHeight;
    bool pretty = false;
};

bool parseOptions(const std::vector<std::string>& args, Options& options) {
    auto takeValue = [&](size_t& i, const char* name, std::string& out) {
        if (i + 1 >= args.size()) {
            std::cerr << "validate-ui: " << name << " needs a value\n";
            return false;
        }
        out = args[++i];
        return true;
    };

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--project") {
            if (!takeValue(i, "--project", options.projectRoot)) return false;
        } else if (a == "--size") {
            std::string size;
            if (!takeValue(i, "--size", size)) return false;
            std::string error;
            if (!parseSize(size, options.width, options.height, error)) {
                std::cerr << "validate-ui: --size " << error << "\n";
                return false;
            }
        } else if (a == "--pretty") {
            options.pretty = true;
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "validate-ui: unknown option '" << a << "'\n";
            return false;
        } else if (options.document.empty()) {
            options.document = a;
        } else {
            std::cerr << "validate-ui: unexpected extra argument '" << a << "'\n";
            return false;
        }
    }
    return true;
}

} // namespace

int runValidateUiCommand(const std::vector<std::string>& args) {
    Options options;
    if (!parseOptions(args, options)) return kExitUsage;

    if (options.document.empty()) {
        std::cerr << "validate-ui: missing <document> (a project-relative .html/.rml path)\n";
        return kExitUsage;
    }
    if (options.projectRoot.empty()) {
        std::cerr << "validate-ui: missing --project <dir>\n";
        return kExitUsage;
    }

    std::vector<Issue> issues;
    std::string reportText;

    {
        UiDocumentSession session;
        std::string error;
        bool documentRejected = false;
        if (!session.open(options.projectRoot, options.document, options.width, options.height,
                          error, documentRejected)) {
            std::cerr << "validate-ui: " << error << "\n";
            for (const RmlUiDiagnostic& diagnostic : session.diagnostics()) {
                if (diagnostic.severity == "info") continue;
                std::cerr << "validate-ui: [" << diagnostic.severity << "] " << diagnostic.message
                          << "\n";
            }
            return documentRejected ? kExitInvalid : kExitUsage;
        }

        const std::filesystem::path projectRoot =
            std::filesystem::absolute(options.projectRoot);

        collectReportedDiagnostics(session, projectRoot, issues);
        checkInlineBoxProperties(session.document(), issues);

        // The text checks run over the document and every stylesheet it pulled
        // in, because a declaration RCSS silently dropped left no trace in the
        // live document to inspect.
        for (const std::string& dependency : session.dependencies()) {
            const std::filesystem::path path(dependency);
            const std::string extension = path.extension().string();
            const bool textual = extension == ".rcss" || extension == ".css" ||
                                 extension == ".rml" || extension == ".html";
            if (!textual) continue;

            std::string contents;
            if (!readFile(dependency, contents)) continue;

            std::error_code ec;
            std::filesystem::path relative =
                std::filesystem::relative(path, projectRoot, ec);
            const std::string label =
                (ec || relative.empty()) ? path.filename().string() : relative.generic_string();
            checkFractionalRgbaAlpha(label, contents, issues);
        }

        // RmlUi raises the same complaint once per layout pass; an author needs
        // to know a problem exists, not how many times it was noticed.
        std::vector<Issue> unique;
        for (const Issue& issue : issues) {
            const bool seen = std::any_of(unique.begin(), unique.end(), [&](const Issue& other) {
                return other.kind == issue.kind && other.where == issue.where &&
                       other.message == issue.message;
            });
            if (!seen) unique.push_back(issue);
        }
        issues = std::move(unique);

        json reported = json::array();
        for (const Issue& issue : issues) {
            reported.push_back(json{{"kind", issue.kind},
                                    {"where", issue.where},
                                    {"message", issue.message}});
        }
        json report{{"ok", issues.empty()},
                    {"document", session.relativePath()},
                    {"size", json{{"width", options.width}, {"height", options.height}}},
                    {"issueCount", issues.size()},
                    {"issues", std::move(reported)}};
        reportText = (options.pretty ? report.dump(2) : report.dump()) + "\n";
    }

    std::cout << reportText;
    return issues.empty() ? kExitOk : kExitInvalid;
}

} // namespace saida
