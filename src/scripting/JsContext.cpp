#include "scripting/JsContext.hpp"
#include "scripting/JsEngineBindings.hpp"

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "scripting/JsRuntime.hpp"

#include <nlohmann/json.hpp>
#include <quickjs.h>

#include <unordered_map>

namespace saida {

namespace {
// Raw QuickJS context → owning wrapper, so native bindings can reach the wrapper
// to register signal subscriptions (the QuickJS opaque already holds the
// Behaviour pointer, so we cannot stash the wrapper there too).
std::unordered_map<JSContext*, JsContext*>& contextRegistry() {
    static std::unordered_map<JSContext*, JsContext*> registry;
    return registry;
}

class JsExecutionScope {
public:
    explicit JsExecutionScope(JsRuntime& runtime) : runtime_(runtime) {
        runtime_.beginExecution();
    }
    ~JsExecutionScope() { runtime_.endExecution(); }

    JsExecutionScope(const JsExecutionScope&) = delete;
    JsExecutionScope& operator=(const JsExecutionScope&) = delete;

private:
    JsRuntime& runtime_;
};

JSValue jsConsoleLog(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    std::string out;
    for (int i = 0; i < argc; ++i) {
        const char* str = JS_ToCString(ctx, argv[i]);
        if (i > 0) out += " ";
        out += str ? str : "<value>";
        JS_FreeCString(ctx, str);
    }

    if (magic == 1) Log::warn("[JS] ", out);
    else if (magic == 2) Log::error("[JS] ", out);
    else Log::info("[JS] ", out);
    return JS_UNDEFINED;
}

JSValue jsonToJsValue(JSContext* ctx, const nlohmann::json& value) {
    if (value.is_null()) return JS_NULL;
    if (value.is_boolean()) return JS_NewBool(ctx, value.get<bool>());
    if (value.is_number_integer()) return JS_NewInt64(ctx, value.get<int64_t>());
    if (value.is_number_unsigned())
        return JS_NewBigUint64(ctx, value.get<uint64_t>());
    if (value.is_number_float()) return JS_NewFloat64(ctx, value.get<double>());
    if (value.is_string()) return JS_NewString(ctx, value.get_ref<const std::string&>().c_str());
    if (value.is_array()) {
        JSValue array = JS_NewArray(ctx);
        uint32_t index = 0;
        for (const auto& item : value)
            JS_SetPropertyUint32(ctx, array, index++, jsonToJsValue(ctx, item));
        return array;
    }
    JSValue object = JS_NewObject(ctx);
    for (auto it = value.begin(); it != value.end(); ++it)
        JS_SetPropertyStr(ctx, object, it.key().c_str(), jsonToJsValue(ctx, it.value()));
    return object;
}

bool jsValueToJson(JSContext* ctx, JSValueConst value, nlohmann::json& out) {
    if (JS_IsUndefined(value)) {
        out = nullptr;
        return true;
    }
    JSValue encoded = JS_JSONStringify(ctx, value, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(encoded)) {
        JS_FreeValue(ctx, encoded);
        return false;
    }
    if (JS_IsUndefined(encoded)) {
        JS_FreeValue(ctx, encoded);
        out = nullptr;
        return true;
    }
    const char* text = JS_ToCString(ctx, encoded);
    if (!text) {
        JS_FreeValue(ctx, encoded);
        return false;
    }
    out = nlohmann::json::parse(text, nullptr, false);
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, encoded);
    return !out.is_discarded();
}
} // namespace

JsContext::JsContext(JsRuntime& runtime)
    : runtime_(runtime), moduleRoot_(activeProjectRoot()) {
    ctx_ = JS_NewContext(runtime_.raw());
    contextRegistry()[ctx_] = this;
    installConsole();
}

JsContext::~JsContext() {
    if (ctx_) {
        // Drop signal subscriptions BEFORE freeing the context: disconnect first
        // (so a pending emit can't reach a freed callback), then release the
        // retained JS callbacks while the context is still valid.
        clearSignalSubscriptions();
        JsEngineBindings::dropPendingFlushes(ctx_);  // storage.flush() en vol
        contextRegistry().erase(ctx_);
        JS_FreeValue(ctx_, moduleNamespace_);
        JS_FreeContext(ctx_);
    }
}

JsContext* JsContext::fromRaw(JSContext* ctx) {
    auto& reg = contextRegistry();
    auto it = reg.find(ctx);
    return it != reg.end() ? it->second : nullptr;
}

void JsContext::retainSignalSubscription(Connection&& connection, JSValue callback) {
    signalSubs_.push_back({std::move(connection), callback});
}

void JsContext::clearSignalSubscriptions() {
    for (auto& sub : signalSubs_) {
        sub.connection.disconnect();
        JS_FreeValue(ctx_, sub.callback);
    }
    signalSubs_.clear();
}

bool JsContext::executePendingJobs() { return runtime_.executePendingJobs(); }

bool JsContext::eval(const std::string& source, const std::string& filename) {
    JSValue value;
    {
        JsExecutionScope execution(runtime_);
        value = JS_Eval(ctx_, source.c_str(), source.size(), filename.c_str(),
                        JS_EVAL_TYPE_GLOBAL);
    }
    if (JS_IsException(value)) {
        JS_FreeValue(ctx_, value);
        reportException(filename);
        return false;
    }
    JS_FreeValue(ctx_, value);
    return runtime_.executePendingJobs();
}

bool JsContext::evalModule(const std::string& source, const std::string& filename) {
    JS_FreeValue(ctx_, moduleNamespace_);
    moduleNamespace_ = JS_UNDEFINED;

    JSValue compiled;
    {
        JsExecutionScope execution(runtime_);
        compiled = JS_Eval(ctx_, source.c_str(), source.size(), filename.c_str(),
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    }
    if (JS_IsException(compiled)) {
        JS_FreeValue(ctx_, compiled);
        reportException(filename);
        return false;
    }

    if (JS_VALUE_GET_TAG(compiled) != JS_TAG_MODULE) {
        JS_FreeValue(ctx_, compiled);
        Log::error("[JS] ", filename, ": compiled value is not a module");
        return false;
    }

    auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
    int resolveResult = 0;
    {
        JsExecutionScope execution(runtime_);
        resolveResult = JS_ResolveModule(ctx_, compiled);
    }
    if (resolveResult < 0) {
        JS_FreeValue(ctx_, compiled);
        reportException(filename);
        return false;
    }

    JSValue result;
    {
        JsExecutionScope execution(runtime_);
        result = JS_EvalFunction(ctx_, compiled);
    }
    if (JS_IsException(result)) {
        JS_FreeValue(ctx_, result);
        reportException(filename);
        return false;
    }
    JS_FreeValue(ctx_, result);

    moduleNamespace_ = JS_GetModuleNamespace(ctx_, module);
    if (JS_IsException(moduleNamespace_)) {
        JS_FreeValue(ctx_, moduleNamespace_);
        moduleNamespace_ = JS_UNDEFINED;
        reportException(filename);
        return false;
    }

    return runtime_.executePendingJobs();
}

bool JsContext::compileCheck(const std::string& source, const std::string& filename,
                             bool asModule, std::string& errorOut) {
    int flags = JS_EVAL_FLAG_COMPILE_ONLY | (asModule ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
    JSValue compiled;
    {
        JsExecutionScope execution(runtime_);
        compiled = JS_Eval(ctx_, source.c_str(), source.size(), filename.c_str(), flags);
    }
    bool ok = !JS_IsException(compiled);
    if (!ok) {
        JSValue exc = JS_GetException(ctx_);
        const char* message = JS_ToCString(ctx_, exc);
        errorOut = message ? message : "unknown compile error";
        JS_FreeCString(ctx_, message);
        JS_FreeValue(ctx_, exc);
    }
    JS_FreeValue(ctx_, compiled);
    return ok;
}

bool JsContext::callGlobal(const char* functionName) {
    return callGlobalImpl(functionName, 0, nullptr);
}

bool JsContext::callGlobal(const char* functionName, double arg) {
    JSValue value = JS_NewFloat64(ctx_, arg);
    bool ok = callGlobalImpl(functionName, 1, &value);
    JS_FreeValue(ctx_, value);
    return ok;
}

bool JsContext::callModuleExport(const char* functionName) {
    return callModuleExportImpl(functionName, 0, nullptr);
}

bool JsContext::callModuleExport(const char* functionName, double arg) {
    JSValue value = JS_NewFloat64(ctx_, arg);
    bool ok = callModuleExportImpl(functionName, 1, &value);
    JS_FreeValue(ctx_, value);
    return ok;
}

bool JsContext::callGlobalJson(const char* functionName, const nlohmann::json& args,
                               nlohmann::json& result) {
    JSValue global = JS_GetGlobalObject(ctx_);
    const bool ok = callJsonImpl(global, functionName, args, result, "global");
    JS_FreeValue(ctx_, global);
    return ok;
}

bool JsContext::callModuleExportJson(const char* functionName,
                                     const nlohmann::json& args,
                                     nlohmann::json& result) {
    if (JS_IsUndefined(moduleNamespace_) || JS_IsNull(moduleNamespace_)) return false;
    return callJsonImpl(moduleNamespace_, functionName, args, result, "module export");
}

JsFunctionStatus JsContext::globalFunctionStatus(const char* functionName) const {
    JSValue global = JS_GetGlobalObject(ctx_);
    JSValue value = JS_GetPropertyStr(ctx_, global, functionName);
    JsFunctionStatus status = JsFunctionStatus::Invalid;
    if (JS_IsUndefined(value) || JS_IsNull(value)) status = JsFunctionStatus::Missing;
    else if (JS_IsFunction(ctx_, value)) status = JsFunctionStatus::Callable;
    JS_FreeValue(ctx_, value);
    JS_FreeValue(ctx_, global);
    return status;
}

JsFunctionStatus JsContext::moduleExportFunctionStatus(const char* functionName) const {
    if (JS_IsUndefined(moduleNamespace_) || JS_IsNull(moduleNamespace_))
        return JsFunctionStatus::Missing;

    JSValue value = JS_GetPropertyStr(ctx_, moduleNamespace_, functionName);
    JsFunctionStatus status = JsFunctionStatus::Invalid;
    if (JS_IsUndefined(value) || JS_IsNull(value)) status = JsFunctionStatus::Missing;
    else if (JS_IsFunction(ctx_, value)) status = JsFunctionStatus::Callable;
    JS_FreeValue(ctx_, value);
    return status;
}

void JsContext::setOpaque(void* opaque) {
    JS_SetContextOpaque(ctx_, opaque);
}

void JsContext::installConsole() {
    JSValue global = JS_GetGlobalObject(ctx_);
    JSValue console = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, console, "log", JS_NewCFunctionMagic(ctx_, jsConsoleLog, "log", 1, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx_, console, "warn", JS_NewCFunctionMagic(ctx_, jsConsoleLog, "warn", 1, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx_, console, "error", JS_NewCFunctionMagic(ctx_, jsConsoleLog, "error", 1, JS_CFUNC_generic_magic, 2));
    JS_SetPropertyStr(ctx_, global, "console", console);
    JS_FreeValue(ctx_, global);
}

bool JsContext::callGlobalImpl(const char* functionName, int argc, JSValue* argv) {
    JSValue global = JS_GetGlobalObject(ctx_);
    JSValue fn = JS_GetPropertyStr(ctx_, global, functionName);
    if (JS_IsUndefined(fn) || JS_IsNull(fn)) {
        JS_FreeValue(ctx_, fn);
        JS_FreeValue(ctx_, global);
        return true;
    }
    if (!JS_IsFunction(ctx_, fn)) {
        Log::warn("[JS] global '", functionName, "' is not a function");
        JS_FreeValue(ctx_, fn);
        JS_FreeValue(ctx_, global);
        return false;
    }

    JSValue result;
    {
        JsExecutionScope execution(runtime_);
        result = JS_Call(ctx_, fn, global, argc, argv);
    }
    JS_FreeValue(ctx_, fn);
    JS_FreeValue(ctx_, global);
    if (JS_IsException(result)) {
        JS_FreeValue(ctx_, result);
        reportException(functionName);
        return false;
    }
    JS_FreeValue(ctx_, result);
    return runtime_.executePendingJobs();
}

bool JsContext::callModuleExportImpl(const char* functionName, int argc, JSValue* argv) {
    if (JS_IsUndefined(moduleNamespace_) || JS_IsNull(moduleNamespace_)) {
        return true;
    }

    JSValue fn = JS_GetPropertyStr(ctx_, moduleNamespace_, functionName);
    if (JS_IsUndefined(fn) || JS_IsNull(fn)) {
        JS_FreeValue(ctx_, fn);
        return true;
    }
    if (!JS_IsFunction(ctx_, fn)) {
        Log::warn("[JS] module export '", functionName, "' is not a function");
        JS_FreeValue(ctx_, fn);
        return false;
    }

    JSValue result;
    {
        JsExecutionScope execution(runtime_);
        result = JS_Call(ctx_, fn, moduleNamespace_, argc, argv);
    }
    JS_FreeValue(ctx_, fn);
    if (JS_IsException(result)) {
        JS_FreeValue(ctx_, result);
        reportException(functionName);
        return false;
    }
    JS_FreeValue(ctx_, result);
    return runtime_.executePendingJobs();
}

bool JsContext::callJsonImpl(JSValueConst owner, const char* functionName,
                             const nlohmann::json& args, nlohmann::json& result,
                             const char* diagnosticKind) {
    JSValue fn = JS_GetPropertyStr(ctx_, owner, functionName);
    if (!JS_IsFunction(ctx_, fn)) {
        Log::warn("[JS] ", diagnosticKind, " '", functionName,
                  "' is missing or is not a function");
        JS_FreeValue(ctx_, fn);
        return false;
    }
    if (!args.is_array()) {
        JS_FreeValue(ctx_, fn);
        Log::error("[JS] cross-context arguments must be a JSON array");
        return false;
    }

    std::vector<JSValue> jsArgs;
    jsArgs.reserve(args.size());
    for (const auto& arg : args) jsArgs.push_back(jsonToJsValue(ctx_, arg));

    JSValue value;
    {
        JsExecutionScope execution(runtime_);
        value = JS_Call(ctx_, fn, owner, static_cast<int>(jsArgs.size()), jsArgs.data());
    }
    JS_FreeValue(ctx_, fn);
    for (JSValue arg : jsArgs) JS_FreeValue(ctx_, arg);

    if (JS_IsException(value)) {
        JS_FreeValue(ctx_, value);
        reportException(functionName);
        return false;
    }
    const bool converted = jsValueToJson(ctx_, value, result);
    JS_FreeValue(ctx_, value);
    if (!converted) {
        reportException(std::string(functionName) + " result serialization");
        return false;
    }
    return runtime_.executePendingJobs();
}

void JsContext::reportException(const std::string& source) {
    JSValue exc = JS_GetException(ctx_);
    const char* message = JS_ToCString(ctx_, exc);
    Log::error("[JS] ", source, ": ", message ? message : "unknown exception");
    JS_FreeCString(ctx_, message);

    if (JS_IsObject(exc)) {
        JSValue stack = JS_GetPropertyStr(ctx_, exc, "stack");
        if (!JS_IsUndefined(stack)) {
            const char* stackText = JS_ToCString(ctx_, stack);
            if (stackText) Log::error("[JS] stack:\n", stackText);
            JS_FreeCString(ctx_, stackText);
        }
        JS_FreeValue(ctx_, stack);
    }

    JS_FreeValue(ctx_, exc);
}

} // namespace saida
