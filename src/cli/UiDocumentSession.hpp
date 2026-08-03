#pragma once

#include "ui/RmlUiRuntime.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Rml {
class Context;
class ElementDocument;
}

namespace saida {

// One headless UI document, open for inspection.
//
// Invariant: while a session exists, the RmlUi runtime is initialized, exactly
// one context and one document are alive, engine log output is routed to stderr
// so stdout stays machine-readable, and every diagnostic and file the document
// pulled in has been recorded. Destroying it tears all of that down in order.
//
// This is what `render-ui` and `validate-ui` both need before they can do
// anything, and the two must agree on it — a document that one of them can open
// and the other cannot would make their verdicts incomparable.
class UiDocumentSession {
public:
    UiDocumentSession();
    ~UiDocumentSession();

    UiDocumentSession(const UiDocumentSession&) = delete;
    UiDocumentSession& operator=(const UiDocumentSession&) = delete;

    // Resolve `documentPath` inside `projectRoot` (project-relative, confined:
    // no absolute path, no parent traversal, no symlink escape), load it at
    // `width` x `height` and lay it out. Returns false with `error` set;
    // `documentRejected` distinguishes a document the engine refused (exit 1)
    // from a bad invocation or missing file (exit 2).
    bool open(const std::string& projectRoot, const std::string& documentPath,
              uint32_t width, uint32_t height, std::string& error, bool& documentRejected);

    Rml::Context& context() const { return *context_; }
    Rml::ElementDocument& document() const { return *document_; }

    // Project-relative path of the loaded document, as it should be reported.
    const std::string& relativePath() const { return relativePath_; }
    const std::string& absolutePath() const { return absolutePath_; }

    // Every diagnostic RmlUi and the engine raised while loading and laying the
    // document out, in order. "info" entries are kept out.
    const std::vector<RmlUiDiagnostic>& diagnostics() const { return diagnostics_; }
    bool hasComplaints() const;

    // Absolute paths of the files the document actually pulled in (itself, its
    // stylesheets, its images). Empty for anything that could not be opened.
    const std::vector<std::string>& dependencies() const { return dependencies_; }

private:
    class OutputGuard;

    std::unique_ptr<OutputGuard> outputGuard_;
    Rml::Context* context_ = nullptr;
    Rml::ElementDocument* document_ = nullptr;
    std::string relativePath_;
    std::string absolutePath_;
    std::vector<RmlUiDiagnostic> diagnostics_;
    std::vector<std::string> dependencies_;
    bool runtimeStarted_ = false;
};

} // namespace saida
