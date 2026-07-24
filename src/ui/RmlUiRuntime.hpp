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

private:
    static bool initialized_;
};

} // namespace saida
