#include "cli/RenderUiCommand.hpp"

#include "core/Paths.hpp"
#include "core/PngWriter.hpp"
#include "ui/RmlUiRenderInterface.hpp"
#include "ui/RmlUiRuntime.hpp"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Types.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace saida {

namespace {

using json = nlohmann::json;

constexpr int kExitOk = 0;
constexpr int kExitInvalid = 1;
constexpr int kExitUsage = 2;

constexpr uint32_t kDefaultWidth = 1920;
constexpr uint32_t kDefaultHeight = 1080;
// A rendered frame is width*height*4 bytes in memory before encoding. The cap
// keeps a mistyped --size from trying to allocate the machine's whole RAM.
constexpr uint32_t kMaxDimension = 16384;

const char* displayName(Rml::Style::Display display) {
    switch (display) {
    case Rml::Style::Display::None: return "none";
    case Rml::Style::Display::Block: return "block";
    case Rml::Style::Display::Inline: return "inline";
    case Rml::Style::Display::InlineBlock: return "inline-block";
    case Rml::Style::Display::FlowRoot: return "flow-root";
    case Rml::Style::Display::Flex: return "flex";
    case Rml::Style::Display::InlineFlex: return "inline-flex";
    case Rml::Style::Display::Table: return "table";
    case Rml::Style::Display::InlineTable: return "inline-table";
    case Rml::Style::Display::TableRow: return "table-row";
    case Rml::Style::Display::TableRowGroup: return "table-row-group";
    case Rml::Style::Display::TableColumn: return "table-column";
    case Rml::Style::Display::TableColumnGroup: return "table-column-group";
    case Rml::Style::Display::TableCell: return "table-cell";
    }
    return "unknown";
}

bool parseSize(const std::string& text, uint32_t& width, uint32_t& height, std::string& error) {
    const size_t separator = text.find_first_of("xX");
    if (separator == std::string::npos || separator == 0 || separator + 1 >= text.size()) {
        error = "expected WxH, got '" + text + "'";
        return false;
    }
    auto parseDimension = [&](const std::string& part, uint32_t& out) {
        unsigned long value = 0;
        const char* begin = part.data();
        const char* end = begin + part.size();
        const std::from_chars_result result = std::from_chars(begin, end, value);
        if (result.ec != std::errc{} || result.ptr != end || value == 0 || value > kMaxDimension) {
            error = "each dimension must be between 1 and " + std::to_string(kMaxDimension) +
                    ", got '" + part + "'";
            return false;
        }
        out = static_cast<uint32_t>(value);
        return true;
    };
    return parseDimension(text.substr(0, separator), width) &&
           parseDimension(text.substr(separator + 1), height);
}

// The computed geometry of one element, in the same coordinates as the
// rendered image, so a layout entry and a pixel can be compared directly.
json describeElement(Rml::Element& element) {
    const Rml::Vector2f offset = element.GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = element.GetBox().GetSize(Rml::BoxArea::Border);
    const Rml::ComputedValues& computed = element.GetComputedValues();

    json entry{
        {"tag", element.GetTagName()},
        {"display", displayName(computed.display())},
        {"visible", element.IsVisible()},
        {"borderBox", json{{"x", offset.x}, {"y", offset.y}, {"width", size.x}, {"height", size.y}}},
    };

    const Rml::String id = element.GetId();
    if (!id.empty()) entry["id"] = id;

    const Rml::String classes = element.GetClassNames();
    if (!classes.empty()) entry["classes"] = classes;

    json children = json::array();
    for (int i = 0; i < element.GetNumChildren(); ++i) {
        if (Rml::Element* child = element.GetChild(i)) {
            children.push_back(describeElement(*child));
        }
    }
    if (!children.empty()) entry["children"] = std::move(children);
    return entry;
}

bool writeText(const std::string& path, const std::string& text, std::string& error) {
    if (path == "-") {
        std::cout << text;
        return true;
    }
    const std::filesystem::path target(path);
    if (target.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
    }
    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot open '" + path + "' for writing";
        return false;
    }
    file << text;
    if (!file) {
        error = "failed while writing '" + path + "'";
        return false;
    }
    return true;
}

// saida_tool keeps machine output on stdout and diagnostics on stderr, but the
// engine's Log::info writes to stdout — font loading alone emits a dozen lines.
// `--layout-json -` must stay parseable, so engine chatter is routed to stderr
// for as long as the engine is running and stdout is left to the report.
class EngineOutputToStderr {
public:
    EngineOutputToStderr() : saved_(std::cout.rdbuf(std::cerr.rdbuf())) {}
    ~EngineOutputToStderr() { std::cout.rdbuf(saved_); }

    EngineOutputToStderr(const EngineOutputToStderr&) = delete;
    EngineOutputToStderr& operator=(const EngineOutputToStderr&) = delete;

private:
    std::streambuf* saved_;
};

struct Options {
    std::string document;
    std::string projectRoot;
    std::string outPng;
    std::string layoutJson;
    uint32_t width = kDefaultWidth;
    uint32_t height = kDefaultHeight;
    bool pretty = false;
    bool allowWarnings = false;
};

bool parseOptions(const std::vector<std::string>& args, Options& options) {
    auto takeValue = [&](size_t& i, const char* name, std::string& out) {
        if (i + 1 >= args.size()) {
            std::cerr << "render-ui: " << name << " needs a value\n";
            return false;
        }
        out = args[++i];
        return true;
    };

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--project") {
            if (!takeValue(i, "--project", options.projectRoot)) return false;
        } else if (a == "--out") {
            if (!takeValue(i, "--out", options.outPng)) return false;
        } else if (a == "--layout-json") {
            if (!takeValue(i, "--layout-json", options.layoutJson)) return false;
        } else if (a == "--size") {
            std::string size;
            if (!takeValue(i, "--size", size)) return false;
            std::string error;
            if (!parseSize(size, options.width, options.height, error)) {
                std::cerr << "render-ui: --size " << error << "\n";
                return false;
            }
        } else if (a == "--pretty") {
            options.pretty = true;
        } else if (a == "--allow-warnings") {
            options.allowWarnings = true;
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "render-ui: unknown option '" << a << "'\n";
            return false;
        } else if (options.document.empty()) {
            options.document = a;
        } else {
            std::cerr << "render-ui: unexpected extra argument '" << a << "'\n";
            return false;
        }
    }
    return true;
}

} // namespace

int runRenderUiCommand(const std::vector<std::string>& args) {
    Options options;
    if (!parseOptions(args, options)) return kExitUsage;

    if (options.document.empty()) {
        std::cerr << "render-ui: missing <document> (a project-relative .html/.rml path)\n";
        return kExitUsage;
    }
    if (options.projectRoot.empty()) {
        std::cerr << "render-ui: missing --project <dir>\n";
        return kExitUsage;
    }
    if (options.outPng.empty() && options.layoutJson.empty()) {
        std::cerr << "render-ui: nothing to produce; pass --out <png> and/or --layout-json <file>\n";
        return kExitUsage;
    }

    std::error_code ec;
    const std::filesystem::path projectRoot = std::filesystem::absolute(options.projectRoot, ec);
    if (ec || !std::filesystem::is_directory(projectRoot)) {
        std::cerr << "render-ui: --project '" << options.projectRoot << "' is not a directory\n";
        return kExitUsage;
    }

    // A document is project content, so it obeys the same confinement as every
    // other tool path (SPEC section 6.2): no absolute path, no traversal, no
    // symlink escape out of the project.
    const SandboxedPathResult resolved =
        resolveSandboxedProjectPath(projectRoot.generic_string(), options.document, "ui");
    if (!resolved) {
        std::cerr << "render-ui: " << resolved.error << "\n";
        return kExitUsage;
    }
    if (!std::filesystem::is_regular_file(resolved.absolute)) {
        std::cerr << "render-ui: no such document '" << resolved.relative << "' under the project\n";
        return kExitUsage;
    }

    setActiveProjectRoot(projectRoot.generic_string());

    int exitCode = kExitOk;
    std::string layoutText;  // produced while the engine runs, written after it stops

    {
        EngineOutputToStderr quiet;

        if (!RmlUiRuntime::ensureInitialized()) {
            std::cerr << "render-ui: the RmlUi runtime failed to initialize\n";
            return kExitUsage;
        }

        std::vector<RmlUiDiagnostic> diagnostics;
        RmlUiRuntime::beginLogCapture(diagnostics);

        Rml::Context* context = Rml::CreateContext(
            "render-ui", {static_cast<int>(options.width), static_cast<int>(options.height)},
            RmlUiRuntime::renderInterface());
        if (context == nullptr) {
            RmlUiRuntime::endLogCapture();
            std::cerr << "render-ui: could not create a rendering context\n";
            return kExitUsage;
        }

        auto reportDiagnostics = [&] {
            for (const RmlUiDiagnostic& diagnostic : diagnostics) {
                if (diagnostic.severity == "info") continue;
                std::cerr << "render-ui: [" << diagnostic.severity << "] " << diagnostic.message
                          << "\n";
            }
        };
        auto abandon = [&](const char* why) {
            RmlUiRuntime::endLogCapture();
            std::cerr << "render-ui: '" << resolved.relative << "' " << why << "\n";
            reportDiagnostics();
            Rml::RemoveContext("render-ui");
            RmlUiRuntime::shutdown();
        };

        Rml::ElementDocument* document = context->LoadDocument(resolved.absolute);
        if (document == nullptr) {
            abandon("could not be loaded");
            return kExitInvalid;
        }
        document->Show();
        context->Update();

        // RmlUi keeps going after a malformed document or a refused declaration:
        // it warns and renders whatever survived. A verification tool must not
        // pass that on as a successful render, so any complaint raised while the
        // document loaded and laid out rejects it — and nothing is written.
        // --allow-warnings is the explicit opt-out for a document whose author
        // has accepted its warnings.
        const bool complained = std::any_of(
            diagnostics.begin(), diagnostics.end(),
            [](const RmlUiDiagnostic& d) { return d.severity != "info"; });
        if (complained && !options.allowWarnings) {
            abandon("was rejected; RmlUi reported (pass --allow-warnings to render anyway)");
            return kExitInvalid;
        }

        if (!options.outPng.empty()) {
            RmlUiRenderInterface* renderer = RmlUiRuntime::renderer();
            renderer->beginFrame(options.width, options.height);
            context->Render();
            renderer->endFrame();

            std::string error;
            if (!writePngRGBA8(options.outPng, renderer->pixels().data(),
                               options.width, options.height, error)) {
                std::cerr << "render-ui: " << error << "\n";
                exitCode = kExitUsage;
            }
        }

        if (exitCode == kExitOk && !options.layoutJson.empty()) {
            json report{
                {"document", resolved.relative},
                {"size", json{{"width", options.width}, {"height", options.height}}},
                {"elements", describeElement(*document)},
            };
            json reported = json::array();
            for (const RmlUiDiagnostic& diagnostic : diagnostics) {
                if (diagnostic.severity == "info") continue;
                reported.push_back(
                    json{{"severity", diagnostic.severity}, {"message", diagnostic.message}});
            }
            report["diagnosticCount"] = reported.size();
            report["diagnostics"] = std::move(reported);
            layoutText = (options.pretty ? report.dump(2) : report.dump()) + "\n";
        }

        RmlUiRuntime::endLogCapture();
        context->UnloadAllDocuments();
        context->Update();
        Rml::RemoveContext("render-ui");
        RmlUiRuntime::shutdown();
    }

    if (exitCode == kExitOk && !layoutText.empty()) {
        std::string error;
        if (!writeText(options.layoutJson, layoutText, error)) {
            std::cerr << "render-ui: " << error << "\n";
            exitCode = kExitUsage;
        }
    }
    return exitCode;
}

} // namespace saida
