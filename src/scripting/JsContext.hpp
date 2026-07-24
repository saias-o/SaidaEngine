#pragma once

#include "core/Signal.hpp"

#include <nlohmann/json_fwd.hpp>
#include <quickjs.h>

#include <string>
#include <vector>

struct JSContext;

namespace saida {

class JsRuntime;

enum class JsFunctionStatus {
    Missing,
    Callable,
    Invalid
};

class JsContext {
public:
    explicit JsContext(JsRuntime& runtime);
    ~JsContext();

    JsContext(const JsContext&) = delete;
    JsContext& operator=(const JsContext&) = delete;

    bool eval(const std::string& source, const std::string& filename = "<eval>");
    bool evalModule(const std::string& source, const std::string& filename);

    // Compile-only syntax check (does not run the script). Returns true if the
    // source compiles; otherwise false with the error message in `errorOut`.
    // `asModule` picks the module vs global parse mode (.mjs vs .js).
    bool compileCheck(const std::string& source, const std::string& filename,
                      bool asModule, std::string& errorOut);
    bool callGlobal(const char* functionName);
    bool callGlobal(const char* functionName, double arg);
    bool callModuleExport(const char* functionName);
    bool callModuleExport(const char* functionName, double arg);
    bool callGlobalJson(const char* functionName, const nlohmann::json& args,
                        nlohmann::json& result);
    bool callModuleExportJson(const char* functionName, const nlohmann::json& args,
                              nlohmann::json& result);
    JsFunctionStatus globalFunctionStatus(const char* functionName) const;
    JsFunctionStatus moduleExportFunctionStatus(const char* functionName) const;

    JSContext* raw() const { return ctx_; }
    void setOpaque(void* opaque);

    // Recover the owning wrapper from a raw QuickJS context (used by native
    // bindings that need to register signal subscriptions). Null if unknown.
    static JsContext* fromRaw(JSContext* ctx);

    // Keep a JS callback subscribed to a reflected signal alive for as long as
    // this context lives. On destruction the connection is dropped and the
    // callback freed — so a hot-reload (which rebuilds the context) cleanly
    // disconnects every `node.on(...)` handler.
    void retainSignalSubscription(Connection&& connection, JSValue callback);

    // Log and consume the exception currently pending on this context.
    // Native asynchronous callbacks use this after JS_Call returns an exception.
    void reportException(const std::string& source);
    bool executePendingJobs();

    // Absolute project/package root captured when the context is created.
    // Module imports are never allowed to escape it.
    const std::string& moduleRoot() const { return moduleRoot_; }

private:
    void installConsole();
    void clearSignalSubscriptions();
    bool callGlobalImpl(const char* functionName, int argc, JSValue* argv);
    bool callModuleExportImpl(const char* functionName, int argc, JSValue* argv);
    bool callJsonImpl(JSValueConst owner, const char* functionName,
                      const nlohmann::json& args, nlohmann::json& result,
                      const char* diagnosticKind);

    struct SignalSubscription {
        Connection connection;
        JSValue callback;
    };

    JsRuntime& runtime_;
    JSContext* ctx_ = nullptr;
    JSValue moduleNamespace_ = JS_UNDEFINED;
    std::vector<SignalSubscription> signalSubs_;
    std::string moduleRoot_;
};

} // namespace saida
