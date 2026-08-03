#pragma once

#include <string>
#include <vector>

namespace Rml {
class RenderInterface;
}

namespace saida {

class RmlUiRenderInterface;

// One default engine font file. `sourcePath` is the resolved location on this
// machine (empty when nothing was found); packaged games must ship every
// `required` file under assets/fonts/ so text renders outside a dev checkout.
struct EngineFontFile {
    std::string fileName;
    std::string sourcePath;
    bool required = true;
};

// One diagnostic emitted by RmlUi while a document was loaded or updated.
// RCSS reports a declaration it could not parse as a warning and then keeps
// going, so these messages are the only trace that a property silently
// vanished; tooling that must report *why* a document looks wrong reads them.
struct RmlUiDiagnostic {
    std::string severity;  // "error" | "warning" | "info"
    std::string message;
};

class RmlUiRuntime {
public:
    static bool ensureInitialized();
    static std::vector<EngineFontFile> engineFontFiles();
    static void shutdown();
    static Rml::RenderInterface* renderInterface();
    static RmlUiRenderInterface* renderer();
    static void beginFileDependencyCapture(std::vector<std::string>& paths);
    static void endFileDependencyCapture();
    static void recordFileDependency(const std::string& pathOrUrl);

    // Collect RmlUi log output into `diagnostics` until endLogCapture(). The
    // messages still reach the engine log; capturing only duplicates them so a
    // caller can attach them to its own report.
    static void beginLogCapture(std::vector<RmlUiDiagnostic>& diagnostics);
    static void endLogCapture();

private:
    static bool initialized_;
};

} // namespace saida
