#include "cli/RenderUiCommand.hpp"

#include "cli/UiCommandSupport.hpp"
#include "cli/UiDocumentSession.hpp"
#include "core/PngWriter.hpp"
#include "ui/RmlUiRenderInterface.hpp"
#include "ui/RmlUiRuntime.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Types.h>

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace saida {

namespace {

using json = nlohmann::json;
using namespace saida::ui_cli;

// The computed geometry of one element, in the same coordinates as the
// rendered image, so a layout entry and a pixel can be compared directly.
json describeElement(Rml::Element& element) {
    const Rml::Vector2f offset = element.GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = element.GetBox().GetSize(Rml::BoxArea::Border);

    json entry{
        {"tag", element.GetTagName()},
        {"display", displayName(element)},
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

void reportDiagnostics(const std::vector<RmlUiDiagnostic>& diagnostics) {
    for (const RmlUiDiagnostic& diagnostic : diagnostics) {
        if (diagnostic.severity == "info") continue;
        std::cerr << "render-ui: [" << diagnostic.severity << "] " << diagnostic.message << "\n";
    }
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

    int exitCode = kExitOk;
    std::string layoutText;  // produced while the engine runs, written after it stops

    {
        UiDocumentSession session;
        std::string error;
        bool documentRejected = false;
        if (!session.open(options.projectRoot, options.document, options.width, options.height,
                          error, documentRejected)) {
            std::cerr << "render-ui: " << error << "\n";
            reportDiagnostics(session.diagnostics());
            return documentRejected ? kExitInvalid : kExitUsage;
        }

        // RmlUi keeps going after a malformed document or a refused declaration:
        // it warns and renders whatever survived. A verification tool must not
        // pass that on as a successful render, so any complaint raised while the
        // document loaded and laid out rejects it — and nothing is written.
        // --allow-warnings is the explicit opt-out for a document whose author
        // has accepted its warnings.
        if (session.hasComplaints() && !options.allowWarnings) {
            std::cerr << "render-ui: '" << session.relativePath()
                      << "' was rejected; RmlUi reported "
                         "(pass --allow-warnings to render anyway)\n";
            reportDiagnostics(session.diagnostics());
            return kExitInvalid;
        }

        if (!options.outPng.empty()) {
            RmlUiRenderInterface* renderer = RmlUiRuntime::renderer();
            renderer->beginFrame(options.width, options.height);
            session.context().Render();
            renderer->endFrame();

            if (!writePngRGBA8(options.outPng, renderer->pixels().data(),
                               options.width, options.height, error)) {
                std::cerr << "render-ui: " << error << "\n";
                exitCode = kExitUsage;
            }
        }

        if (exitCode == kExitOk && !options.layoutJson.empty()) {
            json report{
                {"document", session.relativePath()},
                {"size", json{{"width", options.width}, {"height", options.height}}},
                {"elements", describeElement(session.document())},
            };
            json reported = json::array();
            for (const RmlUiDiagnostic& diagnostic : session.diagnostics()) {
                if (diagnostic.severity == "info") continue;
                reported.push_back(
                    json{{"severity", diagnostic.severity}, {"message", diagnostic.message}});
            }
            report["diagnosticCount"] = reported.size();
            report["diagnostics"] = std::move(reported);
            layoutText = (options.pretty ? report.dump(2) : report.dump()) + "\n";
        }
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
