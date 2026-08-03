#include "cli/UiDocumentSession.hpp"

#include "core/Paths.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>

namespace saida {

namespace {
constexpr const char* kContextName = "saida-ui-session";
}

// saida_tool keeps machine output on stdout and diagnostics on stderr, but the
// engine's Log::info writes to stdout — font loading alone emits a dozen lines.
// Reports must stay parseable on stdout, so engine chatter is routed to stderr
// for as long as the engine is running.
class UiDocumentSession::OutputGuard {
public:
    OutputGuard() : saved_(std::cout.rdbuf(std::cerr.rdbuf())) {}
    ~OutputGuard() { std::cout.rdbuf(saved_); }

    OutputGuard(const OutputGuard&) = delete;
    OutputGuard& operator=(const OutputGuard&) = delete;

private:
    std::streambuf* saved_;
};

UiDocumentSession::UiDocumentSession() = default;

UiDocumentSession::~UiDocumentSession() {
    if (runtimeStarted_) {
        RmlUiRuntime::endLogCapture();
        RmlUiRuntime::endFileDependencyCapture();
        if (context_) {
            context_->UnloadAllDocuments();
            context_->Update();
            Rml::RemoveContext(kContextName);
        }
        RmlUiRuntime::shutdown();
    }
    outputGuard_.reset();
}

bool UiDocumentSession::open(const std::string& projectRoot, const std::string& documentPath,
                             uint32_t width, uint32_t height, std::string& error,
                             bool& documentRejected) {
    documentRejected = false;

    std::error_code ec;
    const std::filesystem::path root = std::filesystem::absolute(projectRoot, ec);
    if (ec || !std::filesystem::is_directory(root)) {
        error = "--project '" + projectRoot + "' is not a directory";
        return false;
    }

    // A document is project content, so it obeys the same confinement as every
    // other tool path (SPEC section 6.2).
    const SandboxedPathResult resolved =
        resolveSandboxedProjectPath(root.generic_string(), documentPath, "ui");
    if (!resolved) {
        error = resolved.error;
        return false;
    }
    if (!std::filesystem::is_regular_file(resolved.absolute)) {
        error = "no such document '" + resolved.relative + "' under the project";
        return false;
    }
    relativePath_ = resolved.relative;
    absolutePath_ = resolved.absolute;

    setActiveProjectRoot(root.generic_string());
    outputGuard_ = std::make_unique<OutputGuard>();

    if (!RmlUiRuntime::ensureInitialized()) {
        error = "the RmlUi runtime failed to initialize";
        return false;
    }
    runtimeStarted_ = true;

    // Capture starts after initialization so the engine's own font loading does
    // not land in a report about the document.
    RmlUiRuntime::beginLogCapture(diagnostics_);
    RmlUiRuntime::beginFileDependencyCapture(dependencies_);

    context_ = Rml::CreateContext(kContextName,
                                  {static_cast<int>(width), static_cast<int>(height)},
                                  RmlUiRuntime::renderInterface());
    if (context_ == nullptr) {
        error = "could not create a rendering context";
        return false;
    }

    document_ = RmlUiRuntime::loadDocument(*context_, absolutePath_);
    if (document_ == nullptr) {
        error = "'" + relativePath_ + "' could not be loaded";
        documentRejected = true;
        return false;
    }
    document_->Show();
    context_->Update();
    return true;
}

bool UiDocumentSession::hasComplaints() const {
    return std::any_of(diagnostics_.begin(), diagnostics_.end(),
                       [](const RmlUiDiagnostic& d) { return d.severity != "info"; });
}

} // namespace saida
