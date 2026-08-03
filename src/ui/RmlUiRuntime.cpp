#include "ui/RmlUiRuntime.hpp"

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "core/Time.hpp"
#include "ui/RmlUiRenderInterface.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/StyleSheetContainer.h>
#include <RmlUi/Core/StyleTypes.h>
#include <RmlUi/Core/SystemInterface.h>

#include <cstdio>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>

namespace saida {

namespace {

std::vector<std::string>* gDependencyCapture = nullptr;
std::vector<RmlUiDiagnostic>* gLogCapture = nullptr;

std::string normalizedPathString(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    std::string out = (ec ? path : absolute).generic_string();
    return out;
}

void recordDependencyPath(const std::filesystem::path& path) {
    if (!gDependencyCapture) return;

    std::string normalized = normalizedPathString(path);
    if (std::find(gDependencyCapture->begin(), gDependencyCapture->end(), normalized) == gDependencyCapture->end()) {
        gDependencyCapture->push_back(std::move(normalized));
    }
}

class NextRmlSystemInterface final : public Rml::SystemInterface {
public:
    double GetElapsedTime() override {
        return static_cast<double>(Time::elapsed());
    }

    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        const char* severity = "info";
        switch (type) {
        case Rml::Log::LT_ERROR:
        case Rml::Log::LT_ASSERT:
            severity = "error";
            Log::error("[RmlUi] ", message);
            break;
        case Rml::Log::LT_WARNING:
            severity = "warning";
            Log::warn("[RmlUi] ", message);
            break;
        default:
            Log::info("[RmlUi] ", message);
            break;
        }
        if (gLogCapture) gLogCapture->push_back({severity, message});
        return true;
    }
};

class NextRmlFileInterface final : public Rml::FileInterface {
public:
    Rml::FileHandle Open(const Rml::String& path) override {
        std::filesystem::path resolved = resolve(path);
        if (isStylesheet(resolved)) {
            std::ifstream in(resolved, std::ios::binary);
            if (!in) return 0;
            std::ostringstream ss;
            ss << in.rdbuf();
            auto* handle = new FileHandle;
            handle->data = sanitizeStylesheet(ss.str());
            recordDependencyPath(resolved);
            return reinterpret_cast<Rml::FileHandle>(handle);
        }

        std::FILE* file = std::fopen(resolved.string().c_str(), "rb");
        if (!file) return 0;
        auto* handle = new FileHandle;
        handle->file = file;
        recordDependencyPath(resolved);
        return reinterpret_cast<Rml::FileHandle>(handle);
    }

    void Close(Rml::FileHandle file) override {
        if (auto* handle = asFile(file)) {
            if (handle->file) std::fclose(handle->file);
            delete handle;
        }
    }

    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override {
        auto* handle = asFile(file);
        if (!handle) return 0;
        if (handle->file) return std::fread(buffer, 1, size, handle->file);
        const size_t remaining = handle->position < handle->data.size() ? handle->data.size() - handle->position : 0;
        const size_t bytes = std::min(size, remaining);
        if (bytes > 0) {
            std::memcpy(buffer, handle->data.data() + handle->position, bytes);
            handle->position += bytes;
        }
        return bytes;
    }

    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        auto* handle = asFile(file);
        if (!handle) return false;
        if (handle->file) return std::fseek(handle->file, offset, origin) == 0;

        long base = 0;
        if (origin == SEEK_CUR) base = static_cast<long>(handle->position);
        else if (origin == SEEK_END) base = static_cast<long>(handle->data.size());
        long next = base + offset;
        if (next < 0) return false;
        handle->position = std::min(static_cast<size_t>(next), handle->data.size());
        return true;
    }

    size_t Tell(Rml::FileHandle file) override {
        auto* handle = asFile(file);
        if (!handle) return 0;
        if (!handle->file) return handle->position;
        long pos = std::ftell(handle->file);
        return pos < 0 ? 0 : static_cast<size_t>(pos);
    }

private:
    struct FileHandle {
        std::FILE* file = nullptr;
        std::string data;
        size_t position = 0;
    };

    static FileHandle* asFile(Rml::FileHandle file) {
        return reinterpret_cast<FileHandle*>(file);
    }

    static bool isStylesheet(const std::filesystem::path& path) {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return ext == ".rcss" || ext == ".css";
    }

    static std::string trim(std::string value) {
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
        return value;
    }

    static bool isUnsupportedWebCssProperty(std::string name) {
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        name = trim(std::move(name));
        return name == "text-shadow"
            || name == "font-smoothing"
            || name == "-webkit-font-smoothing"
            || name == "-moz-osx-font-smoothing";
    }

    static std::string sanitizeStylesheet(const std::string& css) {
        std::string out;
        out.reserve(css.size());
        size_t start = 0;
        while (start < css.size()) {
            size_t end = css.find(';', start);
            size_t tokenEnd = end == std::string::npos ? css.size() : end + 1;
            std::string token = css.substr(start, tokenEnd - start);
            size_t colon = token.find(':');
            size_t openBrace = token.find('{');
            if (colon != std::string::npos && (openBrace == std::string::npos || openBrace < colon)) {
                std::string name = token.substr(openBrace == std::string::npos ? 0 : openBrace + 1,
                                                colon - (openBrace == std::string::npos ? 0 : openBrace + 1));
                if (isUnsupportedWebCssProperty(name)) {
                    out += "/* ignored unsupported web css: ";
                    out += trim(token);
                    out += " */";
                } else {
                    out += token;
                }
            } else {
                out += token;
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return out;
    }

    static std::filesystem::path resolve(const Rml::String& path) {
        std::string normalized = pathFromFileUrl(path);
        std::filesystem::path candidate(normalized);
        if (candidate.is_absolute() && std::filesystem::exists(candidate)) {
            return candidate;
        }
        if (std::filesystem::exists(candidate)) {
            return std::filesystem::absolute(candidate);
        }

        // UI documents, their stylesheets and images belong to the loaded
        // project, not to the engine root assetPath() points at in the editor.
        std::filesystem::path contentCandidate(projectContentPath(normalized));
        if (std::filesystem::exists(contentCandidate)) {
            return contentCandidate;
        }

        return candidate;
    }
};

std::unique_ptr<NextRmlSystemInterface> gSystemInterface;
std::unique_ptr<NextRmlFileInterface> gFileInterface;
std::unique_ptr<RmlUiRenderInterface> gRenderInterface;

// ── Engine user-agent stylesheet ─────────────────────────────────────────────
//
// RmlUi provides no default stylesheet, and RCSS defaults `display` to `inline`
// (StyleSheetSpecification.cpp), so a <div> that does not declare its display is
// an inline box that silently discards width, height, padding and text-align.
// Markup written the way HTML is normally written therefore collapses into a
// left-aligned run of text with nothing logged.
//
// This sheet is the engine's authoring baseline (SPEC section 8.2): it restores
// the block/inline-block distinction HTML authors already assume, and nothing
// else. It is a contract, not a theme — no margins, no fonts, no colours — and
// it is embedded rather than shipped as a file so it can never go missing from
// a package. It is merged *under* each document, so any rule the document
// declares wins.
constexpr const char* kUserAgentStyleSheet = R"RCSS(
body, div, p, section, h1, h2, h3, h4, h5, h6 { display: block; }
img, button { display: inline-block; }
)RCSS";

Rml::SharedPtr<Rml::StyleSheetContainer> gUserAgentSheet;

// Merge the user-agent sheet under `document`'s own rules. Failing to build the
// sheet is an engine defect, not a document problem: report it rather than
// letting documents silently fall back to RCSS's inline default.
void applyUserAgentStyleSheet(Rml::ElementDocument& document) {
    if (!gUserAgentSheet) {
        gUserAgentSheet = Rml::Factory::InstanceStyleSheetString(kUserAgentStyleSheet);
        if (!gUserAgentSheet) {
            Log::error("[RmlUi] the engine user-agent stylesheet failed to parse — "
                       "documents will use the RCSS inline default");
            return;
        }
    }

    const Rml::StyleSheetContainer* own = document.GetStyleSheetContainer();
    document.SetStyleSheetContainer(
        own ? gUserAgentSheet->CombineStyleSheetContainer(*own)
            : gUserAgentSheet->CombineStyleSheetContainer(Rml::StyleSheetContainer()));
}

// ── Default engine fonts ─────────────────────────────────────────────────────
//
// The game declares its own fonts via @font-face in its RCSS stylesheets; this
// manifest only covers the default families (sans-serif, monospace, emoji
// fallback) that any text must be able to render without configuration.
// File resolution: assets/fonts/ (packaged bundle, runtime root) first, then
// the dev checkout (sample fonts from the RmlUi submodule).

struct EngineFontSpec {
    const char* fileName;
    Rml::Style::FontWeight weight;
    bool fallback;      // participates in the glyph fallback chain
    bool requiredOnWeb; // false = declared limitation of the Web player (SPEC §14)
};

// NotoEmoji stays out of the wasm bundles: 400 KiB extra at boot for a glyph
// fallback that V1 content doesn't use.
constexpr EngineFontSpec kEngineFonts[] = {
    {"LatoLatin-Regular.ttf", Rml::Style::FontWeight::Normal, true, true},
    {"LatoLatin-Bold.ttf", Rml::Style::FontWeight::Bold, false, true},
    {"RobotoMono-Regular.ttf", Rml::Style::FontWeight::Normal, false, true},
    {"NotoEmoji-Regular.ttf", Rml::Style::FontWeight::Normal, true, false},
};

constexpr const char* kEngineFontRoots[] = {
    "assets/fonts/",
    "third_party/rmlui/Samples/assets/",
};

// The generic families that documents reference; all point to the files
// above. "NextSans" is the public family name used by HUDs.
constexpr const char* kSansSerifAliases[] = {"NextSans", "Arial", "Helvetica", "sans-serif"};

bool isFontRequiredHere(const EngineFontSpec& spec) {
#ifdef __EMSCRIPTEN__
    return spec.requiredOnWeb;
#else
    (void)spec;
    return true;
#endif
}

std::string resolveEngineFont(const char* fileName) {
    for (const char* root : kEngineFontRoots) {
        std::string candidate = assetPath(std::string(root) + fileName);
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return {};
}

bool loadFontFace(const std::string& path, bool fallback, Rml::Style::FontWeight weight) {
    if (Rml::LoadFontFace(path, fallback, weight)) return true;
    Log::error("[RmlUi] failed to load font: ", path);
    return false;
}

bool loadFontAlias(const std::string& path, const std::string& family,
                   Rml::Style::FontWeight weight, bool fallback = false) {
    if (Rml::LoadFontFace(path, family, Rml::Style::FontStyle::Normal, weight, fallback)) return true;
    Log::error("[RmlUi] failed to load font alias ", family, ": ", path);
    return false;
}

void loadDefaultFonts() {
    int loadedFaces = 0;
    std::string latoRegular;
    std::string latoBold;
    std::string monoRegular;
    for (const EngineFontSpec& spec : kEngineFonts) {
        const std::string resolved = resolveEngineFont(spec.fileName);
        if (resolved.empty()) {
            if (isFontRequiredHere(spec)) {
                Log::error("[RmlUi] engine font missing: ", spec.fileName,
                           " (searched assets/fonts/ then the dev checkout)");
            } else {
                Log::info("[RmlUi] optional engine font not bundled: ", spec.fileName);
            }
            continue;
        }
        if (loadFontFace(resolved, spec.fallback, spec.weight)) ++loadedFaces;
        if (spec.fileName == std::string("LatoLatin-Regular.ttf")) latoRegular = resolved;
        else if (spec.fileName == std::string("LatoLatin-Bold.ttf")) latoBold = resolved;
        else if (spec.fileName == std::string("RobotoMono-Regular.ttf")) monoRegular = resolved;
    }

    for (const char* family : kSansSerifAliases) {
        const bool isGenericFallback = family == std::string("sans-serif");
        if (!latoRegular.empty()) loadFontAlias(latoRegular, family, Rml::Style::FontWeight::Normal, isGenericFallback);
        if (!latoBold.empty()) loadFontAlias(latoBold, family, Rml::Style::FontWeight::Bold);
    }
    if (!monoRegular.empty()) loadFontAlias(monoRegular, "monospace", Rml::Style::FontWeight::Normal);

    if (loadedFaces == 0) {
        Log::error("[RmlUi] no engine font could be loaded — UI text will not render");
    }
}

} // namespace

bool RmlUiRuntime::initialized_ = false;

std::vector<EngineFontFile> RmlUiRuntime::engineFontFiles() {
    std::vector<EngineFontFile> files;
    files.reserve(std::size(kEngineFonts));
    for (const EngineFontSpec& spec : kEngineFonts) {
        files.push_back({spec.fileName, resolveEngineFont(spec.fileName), isFontRequiredHere(spec)});
    }
    return files;
}

bool RmlUiRuntime::ensureInitialized() {
    if (initialized_) return true;

    gSystemInterface = std::make_unique<NextRmlSystemInterface>();
    gFileInterface = std::make_unique<NextRmlFileInterface>();
    gRenderInterface = std::make_unique<RmlUiRenderInterface>();

    Rml::SetSystemInterface(gSystemInterface.get());
    Rml::SetFileInterface(gFileInterface.get());
    Rml::SetRenderInterface(gRenderInterface.get());

    initialized_ = Rml::Initialise();
    if (!initialized_) {
        Log::error("[RmlUi] initialization failed");
        gRenderInterface.reset();
        gFileInterface.reset();
        gSystemInterface.reset();
    } else {
        loadDefaultFonts();
    }
    return initialized_;
}

void RmlUiRuntime::shutdown() {
    if (!initialized_) return;
    // Released before Rml::Shutdown(): the sheet is an RmlUi object and must not
    // outlive the core that owns its allocator.
    gUserAgentSheet.reset();
    Rml::Shutdown();
    initialized_ = false;
    gRenderInterface.reset();
    gFileInterface.reset();
    gSystemInterface.reset();
}

Rml::RenderInterface* RmlUiRuntime::renderInterface() {
    return gRenderInterface.get();
}

RmlUiRenderInterface* RmlUiRuntime::renderer() {
    return gRenderInterface.get();
}

void RmlUiRuntime::beginFileDependencyCapture(std::vector<std::string>& paths) {
    paths.clear();
    gDependencyCapture = &paths;
}

void RmlUiRuntime::endFileDependencyCapture() {
    gDependencyCapture = nullptr;
}

const char* RmlUiRuntime::userAgentStyleSheet() {
    return kUserAgentStyleSheet;
}

Rml::ElementDocument* RmlUiRuntime::loadDocument(Rml::Context& context, const std::string& path) {
    Rml::ElementDocument* document = context.LoadDocument(path);
    if (document) applyUserAgentStyleSheet(*document);
    return document;
}

Rml::ElementDocument* RmlUiRuntime::loadDocumentFromMemory(Rml::Context& context,
                                                           const std::string& markup,
                                                           const std::string& sourceName) {
    Rml::ElementDocument* document = context.LoadDocumentFromMemory(markup, sourceName);
    if (document) applyUserAgentStyleSheet(*document);
    return document;
}

void RmlUiRuntime::beginLogCapture(std::vector<RmlUiDiagnostic>& diagnostics) {
    diagnostics.clear();
    gLogCapture = &diagnostics;
}

void RmlUiRuntime::endLogCapture() {
    gLogCapture = nullptr;
}

void RmlUiRuntime::recordFileDependency(const std::string& pathOrUrl) {
    if (!gDependencyCapture) return;

    std::string normalized = pathFromFileUrl(pathOrUrl);
    std::filesystem::path candidate(normalized);
    if (candidate.is_absolute() && std::filesystem::exists(candidate)) {
        recordDependencyPath(candidate);
    } else if (std::filesystem::exists(candidate)) {
        recordDependencyPath(candidate);
    } else {
        std::filesystem::path assetCandidate(assetPath(normalized));
        if (std::filesystem::exists(assetCandidate)) recordDependencyPath(assetCandidate);
    }
}

} // namespace saida
