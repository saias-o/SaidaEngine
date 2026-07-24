#include "scripting/JsRuntime.hpp"

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "scripting/JsContext.hpp"

#include <quickjs.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace saida {

namespace {
constexpr size_t kJsMemoryLimit = 64ull * 1024ull * 1024ull;
#ifdef __EMSCRIPTEN__
// The wasm stack is smaller than the native stack; the QuickJS limit must
// stay under the player's STACK_SIZE (4 MiB).
constexpr size_t kJsStackLimit = 256ull * 1024ull;
#else
constexpr size_t kJsStackLimit = 1024ull * 1024ull;
#endif
constexpr auto kJsExecutionBudget = std::chrono::milliseconds(100);
constexpr int kMaxPendingJobsPerDrain = 1024;
std::vector<std::string>* gModuleDependencyCapture = nullptr;

void recordModuleDependency(const std::filesystem::path& path) {
    if (!gModuleDependencyCapture) return;

    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    std::string normalized = (ec ? path : absolute).generic_string();
    if (std::find(gModuleDependencyCapture->begin(), gModuleDependencyCapture->end(), normalized) == gModuleDependencyCapture->end()) {
        gModuleDependencyCapture->push_back(std::move(normalized));
    }
}

std::string lowerForCompare(std::string value) {
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return value;
}

bool isInsideRoot(const std::filesystem::path& root,
                  const std::filesystem::path& candidate) {
    std::string rootText = lowerForCompare(root.generic_string());
    std::string candidateText = lowerForCompare(candidate.generic_string());
    while (rootText.size() > 1 && rootText.back() == '/') rootText.pop_back();
    while (candidateText.size() > 1 && candidateText.back() == '/') candidateText.pop_back();
    return candidateText == rootText ||
           (candidateText.size() > rootText.size() &&
            candidateText.compare(0, rootText.size(), rootText) == 0 &&
            candidateText[rootText.size()] == '/');
}

struct ModulePathResult {
    std::filesystem::path path;
    std::string error;

    explicit operator bool() const { return error.empty(); }
};

ModulePathResult validateModulePath(JSContext* ctx,
                                    const std::filesystem::path& candidate) {
    const JsContext* wrapper = JsContext::fromRaw(ctx);
    if (!wrapper || wrapper->moduleRoot().empty()) {
        return {{}, "module imports require an active project root"};
    }

    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::weakly_canonical(wrapper->moduleRoot(), ec);
    if (ec) return {{}, "could not resolve the project module root"};

    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(candidate, ec);
    const bool regularFile = !ec && std::filesystem::is_regular_file(canonical, ec);
    if (ec || !regularFile) {
        return {{}, "module does not name a readable project file"};
    }
    if (!isInsideRoot(root, canonical)) {
        return {{}, "module import escapes the project root"};
    }

    const std::string extension = lowerForCompare(canonical.extension().string());
    if (extension != ".js" && extension != ".mjs") {
        return {{}, "module imports are limited to .js and .mjs project files"};
    }
    return {canonical, {}};
}

ModulePathResult resolveModulePath(JSContext* ctx, const char* baseName,
                                   const char* moduleName) {
    const std::string raw = pathFromFileUrl(moduleName ? moduleName : "");
    const std::filesystem::path requested(raw);
    if (raw.empty()) return {{}, "module specifier is empty"};
    if (requested.is_absolute() || requested.has_root_name() ||
        requested.has_root_directory() || raw.find(':') != std::string::npos) {
        return {{}, "module specifier must be project-relative"};
    }

    const JsContext* wrapper = JsContext::fromRaw(ctx);
    if (!wrapper || wrapper->moduleRoot().empty()) {
        return {{}, "module imports require an active project root"};
    }

    std::filesystem::path base(pathFromFileUrl(baseName ? baseName : ""));
    std::filesystem::path candidate = std::filesystem::path(wrapper->moduleRoot()) / requested;
    if (!base.empty() && base.has_parent_path()) {
        candidate = base.parent_path() / requested;
    }
    return validateModulePath(ctx, candidate);
}

char* jsModuleNormalize(JSContext* ctx, const char* baseName, const char* moduleName, void*) {
    try {
        const ModulePathResult resolved = resolveModulePath(ctx, baseName, moduleName);
        if (!resolved) {
            JS_ThrowReferenceError(ctx, "%s: '%s'", resolved.error.c_str(),
                                   moduleName ? moduleName : "");
            return nullptr;
        }
        return js_strdup(ctx, resolved.path.generic_string().c_str());
    } catch (const std::exception& error) {
        JS_ThrowReferenceError(ctx, "module resolution failed: %s", error.what());
        return nullptr;
    }
}

JSModuleDef* jsModuleLoader(JSContext* ctx, const char* moduleName, void*) {
    try {
        const ModulePathResult resolved = validateModulePath(
            ctx, std::filesystem::path(pathFromFileUrl(moduleName ? moduleName : "")));
        if (!resolved) {
            JS_ThrowReferenceError(ctx, "%s", resolved.error.c_str());
            return nullptr;
        }

        std::ifstream file(resolved.path);
        if (!file.is_open()) {
            JS_ThrowReferenceError(ctx, "could not load module '%s'",
                                   moduleName ? moduleName : "");
            return nullptr;
        }
        recordModuleDependency(resolved.path);

        std::stringstream ss;
        ss << file.rdbuf();
        std::string source = ss.str();

        JSValue value = JS_Eval(ctx, source.c_str(), source.size(),
                                resolved.path.generic_string().c_str(),
                                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(value)) return nullptr;

        auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(value));
        JS_FreeValue(ctx, value);
        return module;
    } catch (const std::exception& error) {
        JS_ThrowReferenceError(ctx, "module load failed: %s", error.what());
        return nullptr;
    }
}

int jsInterruptHandler(JSRuntime*, void* opaque) {
    auto* runtime = static_cast<JsRuntime*>(opaque);
    return runtime && runtime->shouldInterrupt() ? 1 : 0;
}
}

JsRuntime& JsRuntime::instance() {
    static JsRuntime runtime;
    return runtime;
}

JsRuntime::JsRuntime() {
    runtime_ = JS_NewRuntime();
    JS_SetMemoryLimit(runtime_, kJsMemoryLimit);
    JS_SetMaxStackSize(runtime_, kJsStackLimit);
    JS_SetInterruptHandler(runtime_, jsInterruptHandler, this);
    JS_SetModuleLoaderFunc(runtime_, jsModuleNormalize, jsModuleLoader, nullptr);
    Log::info("QuickJS runtime initialized");
}

JsRuntime::~JsRuntime() {
    if (runtime_) JS_FreeRuntime(runtime_);
    Log::info("QuickJS runtime destroyed");
}

std::unique_ptr<JsContext> JsRuntime::createContext() {
    return std::make_unique<JsContext>(*this);
}

bool JsRuntime::executePendingJobs() {
    beginExecution();
    JSContext* ctx = nullptr;
    int executed = 0;
    int result = 0;
    while (executed < kMaxPendingJobsPerDrain &&
           (result = JS_ExecutePendingJob(runtime_, &ctx)) > 0) {
        ++executed;
    }

    bool ok = result >= 0;
    bool reportJobException = result < 0;
    if (ok && executed == kMaxPendingJobsPerDrain) {
        // Probe once with an immediate interrupt. A zero result means the queue
        // ended exactly at the budget; otherwise the runaway chain is stopped.
        abortRequested_ = true;
        result = JS_ExecutePendingJob(runtime_, &ctx);
        abortRequested_ = false;
        ok = result == 0;
        if (!ok) {
            reportJobException = false;
            Log::error("[JS] pending job budget exceeded (",
                       kMaxPendingJobsPerDrain, ")");
        }
    }
    endExecution();

    if (reportJobException && ctx) {
        if (JsContext* wrapper = JsContext::fromRaw(ctx)) {
            wrapper->reportException("pending job");
        }
    }
    return ok;
}

void JsRuntime::beginModuleDependencyCapture(std::vector<std::string>& paths) {
    paths.clear();
    gModuleDependencyCapture = &paths;
}

void JsRuntime::endModuleDependencyCapture() {
    gModuleDependencyCapture = nullptr;
}

void JsRuntime::beginExecution() {
    if (executionDepth_++ == 0) {
        executionDeadline_ = std::chrono::steady_clock::now() + kJsExecutionBudget;
        abortRequested_ = false;
    }
}

void JsRuntime::endExecution() {
    if (executionDepth_ > 0) --executionDepth_;
    if (executionDepth_ == 0) abortRequested_ = false;
}

bool JsRuntime::shouldInterrupt() {
    if (abortRequested_) return true;
    return executionDepth_ > 0 &&
           std::chrono::steady_clock::now() >= executionDeadline_;
}

} // namespace saida
