#include "scripting/ScriptBehaviour.hpp"

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "core/Profiler.hpp"
#include "core/Reflection.hpp"
#include "scene/SceneTimerQueue.hpp"
#include "scripting/JsContext.hpp"
#include "scripting/JsEngineBindings.hpp"
#include "scripting/JsRuntime.hpp"

#include <nlohmann/json.hpp>
#include <quickjs.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <fstream>
#include <sstream>
#include <utility>

namespace saida {

namespace {

constexpr float kHotReloadCheckIntervalSeconds = 0.5f;

float hotReloadPhase(const std::string& key) {
    if (key.empty()) return 0.0f;
    size_t bucket = std::hash<std::string>{}(key) % 1000u;
    return (static_cast<float>(bucket) / 1000.0f) * kHotReloadCheckIntervalSeconds;
}

class JsModuleDependencyCapture {
public:
    explicit JsModuleDependencyCapture(std::vector<std::string>& dependencies) {
        JsRuntime::instance().beginModuleDependencyCapture(dependencies);
    }

    ~JsModuleDependencyCapture() {
        JsRuntime::instance().endModuleDependencyCapture();
    }

    JsModuleDependencyCapture(const JsModuleDependencyCapture&) = delete;
    JsModuleDependencyCapture& operator=(const JsModuleDependencyCapture&) = delete;
};

ScriptBehaviour* scriptBehaviourFromJs(JSContext* ctx) {
    return dynamic_cast<ScriptBehaviour*>(static_cast<Behaviour*>(JS_GetContextOpaque(ctx)));
}

JSValue jsExportProperty(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* behaviour = scriptBehaviourFromJs(ctx);
    if (!behaviour || argc < 2) return JS_NewBool(ctx, false);

    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name || name[0] == '\0') {
        JS_FreeCString(ctx, name);
        return JS_NewBool(ctx, false);
    }

    bool ok = true;
    if (JS_IsBool(argv[1])) {
        int value = JS_ToBool(ctx, argv[1]);
        if (value < 0) ok = false;
        else behaviour->exportBoolProperty(name, value != 0);
    } else if (JS_IsNumber(argv[1])) {
        double value = 0.0;
        ok = JS_ToFloat64(ctx, &value, argv[1]) == 0;
        if (ok) behaviour->exportNumberProperty(name, value);
    } else if (JS_IsString(argv[1])) {
        const char* value = JS_ToCString(ctx, argv[1]);
        ok = value != nullptr;
        if (ok) behaviour->exportStringProperty(name, value);
        JS_FreeCString(ctx, value);
    } else {
        Log::warn("ScriptBehaviour: exportProperty supports number, boolean and string only");
        ok = false;
    }

    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, ok);
}

JSValue propertyToJs(JSContext* ctx, const ScriptProperty& property) {
    switch (property.type) {
    case ScriptProperty::Type::Number:
        return JS_NewFloat64(ctx, std::get<double>(property.value));
    case ScriptProperty::Type::Boolean:
        return JS_NewBool(ctx, std::get<bool>(property.value));
    case ScriptProperty::Type::String:
        return JS_NewString(ctx, std::get<std::string>(property.value).c_str());
    }
    return JS_UNDEFINED;
}

} // namespace

ScriptBehaviour::~ScriptBehaviour() { cancelAllJsTimers(); }

uint64_t ScriptBehaviour::scheduleJsTimer(JSContext* context, JSValueConst callback,
                                          JsTimerKind kind, float duration,
                                          Easing easing) {
    if (!context) return kInvalidTimerId;

    const uint64_t callbackId = ++nextJsCallbackId_;
    uint64_t timerId = kInvalidTimerId;
    uint64_t cleanupTimerId = kInvalidTimerId;

    switch (kind) {
    case JsTimerKind::Wait:
        timerId = wait(duration, [this, callbackId] { invokeJsTimer(callbackId); });
        break;
    case JsTimerKind::Every:
        timerId = every(duration, [this, callbackId] { invokeJsTimer(callbackId); });
        break;
    case JsTimerKind::Tween:
        timerId = tween(duration, easing, [this, callbackId](float value) {
            invokeJsTimer(callbackId, value);
        });
        // The queue's tween callback intentionally exposes only the eased value,
        // which may overshoot 1.0. A paired one-shot gives cleanup an exact,
        // easing-independent completion point and executes after the final sample.
        if (timerId != kInvalidTimerId) {
            cleanupTimerId = wait(duration, [this, callbackId] {
                removeJsTimer(callbackId, false);
            });
        }
        break;
    }

    if (timerId == kInvalidTimerId) {
        if (cleanupTimerId != kInvalidTimerId) cancelTimer(cleanupTimerId);
        return kInvalidTimerId;
    }

    jsTimers_.push_back({callbackId, timerId, cleanupTimerId, context,
                         JS_DupValue(context, callback), kind});
    return timerId;
}

bool ScriptBehaviour::cancelJsTimer(uint64_t timerId) {
    auto it = std::find_if(jsTimers_.begin(), jsTimers_.end(),
                           [timerId](const JsTimerCallback& timer) {
                               return timer.timerId == timerId;
                           });
    if (it == jsTimers_.end()) return false;
    removeJsTimer(it->callbackId, true);
    return true;
}

void ScriptBehaviour::cancelJsTimersForContext(JSContext* context) {
    for (size_t i = jsTimers_.size(); i > 0; --i) {
        if (jsTimers_[i - 1].context == context)
            removeJsTimer(jsTimers_[i - 1].callbackId, true);
    }
}

void ScriptBehaviour::cancelAllJsTimers() {
    while (!jsTimers_.empty()) removeJsTimer(jsTimers_.back().callbackId, true);
}

void ScriptBehaviour::invokeJsTimer(uint64_t callbackId, float tweenValue) {
    auto it = std::find_if(jsTimers_.begin(), jsTimers_.end(),
                           [callbackId](const JsTimerCallback& timer) {
                               return timer.callbackId == callbackId;
                           });
    if (it == jsTimers_.end() || !it->context) return;

    JSContext* context = it->context;
    const JsTimerKind kind = it->kind;
    JSValue callback = JS_DupValue(context, it->callback);
    JSValue argument = JS_UNDEFINED;
    int argumentCount = 0;
    if (kind == JsTimerKind::Tween) {
        argument = JS_NewFloat64(context, tweenValue);
        argumentCount = 1;
    }

    JSValue result = JS_Call(context, callback, JS_UNDEFINED, argumentCount,
                             argumentCount ? &argument : nullptr);
    if (JS_IsException(result)) {
        if (JsContext* wrapper = JsContext::fromRaw(context))
            wrapper->reportException("time callback");
    }
    JS_FreeValue(context, result);
    if (argumentCount) JS_FreeValue(context, argument);
    JS_FreeValue(context, callback);

    if (JsContext* wrapper = JsContext::fromRaw(context))
        wrapper->executePendingJobs();

    // A callback may cancel itself. Look it up again before releasing a one-shot.
    if (kind == JsTimerKind::Wait) removeJsTimer(callbackId, false);
}

void ScriptBehaviour::removeJsTimer(uint64_t callbackId, bool cancelEngineTimer) {
    auto it = std::find_if(jsTimers_.begin(), jsTimers_.end(),
                           [callbackId](const JsTimerCallback& timer) {
                               return timer.callbackId == callbackId;
                           });
    if (it == jsTimers_.end()) return;

    if (cancelEngineTimer) cancelTimer(it->timerId);
    if (it->cleanupTimerId != kInvalidTimerId) cancelTimer(it->cleanupTimerId);
    if (it->context) JS_FreeValue(it->context, it->callback);
    jsTimers_.erase(it);
}

void ScriptBehaviour::setScriptPath(std::string path) {
    scriptPath_ = std::move(path);
    hotReloadTimer_ = hotReloadPhase(scriptPath_);
    loaded_ = false;
    scriptWatcher_.clear();
    moduleWatchers_.clear();
}

bool ScriptBehaviour::reload() {
    bool ok = reloadContext(started_);
    if (ok && started_ && context_) {
        callHook(LifecycleHook::Ready);
    }
    return ok;
}

ScriptCallStatus ScriptBehaviour::callExport(const std::string& name,
                                             const nlohmann::json& args,
                                             nlohmann::json& result) {
    if (name.empty()) return ScriptCallStatus::Missing;
    if ((!loaded_ || !context_) && !reloadContext(false))
        return ScriptCallStatus::Failed;

    const JsFunctionStatus status = moduleMode_
        ? context_->moduleExportFunctionStatus(name.c_str())
        : context_->globalFunctionStatus(name.c_str());
    if (status == JsFunctionStatus::Missing) return ScriptCallStatus::Missing;
    if (status != JsFunctionStatus::Callable) return ScriptCallStatus::Failed;

    const bool ok = moduleMode_
        ? context_->callModuleExportJson(name.c_str(), args, result)
        : context_->callGlobalJson(name.c_str(), args, result);
    return ok ? ScriptCallStatus::Succeeded : ScriptCallStatus::Failed;
}

bool ScriptBehaviour::reloadContext(bool lifecycleReload) {
    auto previousContext = std::move(context_);
    bool previousLoaded = loaded_;
    bool previousModuleMode = moduleMode_;
    auto previousLifecycleHooks = lifecycleHooks_;
    WatchedFile previousWatcher = scriptWatcher_;
    auto previousProperties = properties_;
    auto previousModuleWatchers = moduleWatchers_;

    std::string path = resolveScriptPath();
    if (path.empty()) {
        Log::warn("ScriptBehaviour: no script path set");
        context_ = std::move(previousContext);
        loaded_ = previousLoaded;
        moduleMode_ = previousModuleMode;
        lifecycleHooks_ = previousLifecycleHooks;
        scriptWatcher_ = previousWatcher;
        moduleWatchers_ = std::move(previousModuleWatchers);
        return false;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        Log::error("ScriptBehaviour: cannot open script: ", path);
        context_ = std::move(previousContext);
        loaded_ = previousLoaded;
        moduleMode_ = previousModuleMode;
        lifecycleHooks_ = previousLifecycleHooks;
        scriptWatcher_ = previousWatcher;
        moduleWatchers_ = std::move(previousModuleWatchers);
        return false;
    }

    std::stringstream ss;
    ss << file.rdbuf();

    preparePropertyReload();
    context_ = JsRuntime::instance().createContext();
    JsEngineBindings::installForBehaviour(*context_, *this);
    installScriptApi();
    applyAllPropertiesToJs();
    moduleMode_ = shouldLoadAsModule(path);
    std::vector<std::string> moduleDependencies;
    if (moduleMode_) {
        JsModuleDependencyCapture capture(moduleDependencies);
        loaded_ = context_->evalModule(ss.str(), path);
    } else {
        loaded_ = context_->eval(ss.str(), path);
    }
    if (!loaded_) {
        if (context_) cancelJsTimersForContext(context_->raw());
        context_ = std::move(previousContext);
        properties_ = std::move(previousProperties);
        loaded_ = previousLoaded;
        moduleMode_ = previousModuleMode;
        lifecycleHooks_ = previousLifecycleHooks;
        scriptWatcher_ = previousWatcher;
        moduleWatchers_ = std::move(previousModuleWatchers);
        return false;
    }

    inspectLifecycleHooks();

    if (previousContext) {
        const auto destroyIndex = static_cast<std::size_t>(LifecycleHook::Destroy);
        if (lifecycleReload && previousLifecycleHooks[destroyIndex]) {
            if (previousModuleMode) previousContext->callModuleExport("onDestroy");
            else previousContext->callGlobal("onDestroy");
        }
        cancelJsTimersForContext(previousContext->raw());
    }

    pruneUnexportedProperties();
    applyAllPropertiesToJs();
    updateScriptWatchers(path, moduleDependencies);
    return true;
}

void ScriptBehaviour::exportNumberProperty(const std::string& name, double defaultValue) {
    if (auto* property = findProperty(name)) {
        property->exportedThisLoad = true;
        if (property->type != ScriptProperty::Type::Number) {
            setPropertyValue(*property, defaultValue);
        }
        applyPropertyToJs(*property);
        return;
    }

    ScriptProperty property;
    property.name = name;
    property.type = ScriptProperty::Type::Number;
    property.value = defaultValue;
    property.exportedThisLoad = true;
    properties_.push_back(std::move(property));
    applyPropertyToJs(properties_.back());
}

void ScriptBehaviour::exportBoolProperty(const std::string& name, bool defaultValue) {
    if (auto* property = findProperty(name)) {
        property->exportedThisLoad = true;
        if (property->type != ScriptProperty::Type::Boolean) {
            setPropertyValue(*property, defaultValue);
        }
        applyPropertyToJs(*property);
        return;
    }

    ScriptProperty property;
    property.name = name;
    property.type = ScriptProperty::Type::Boolean;
    property.value = defaultValue;
    property.exportedThisLoad = true;
    properties_.push_back(std::move(property));
    applyPropertyToJs(properties_.back());
}

void ScriptBehaviour::exportStringProperty(const std::string& name, const std::string& defaultValue) {
    if (auto* property = findProperty(name)) {
        property->exportedThisLoad = true;
        if (property->type != ScriptProperty::Type::String) {
            setPropertyValue(*property, defaultValue);
        }
        applyPropertyToJs(*property);
        return;
    }

    ScriptProperty property;
    property.name = name;
    property.type = ScriptProperty::Type::String;
    property.value = defaultValue;
    property.exportedThisLoad = true;
    properties_.push_back(std::move(property));
    applyPropertyToJs(properties_.back());
}

void ScriptBehaviour::onReady() {
    if (!loaded_ && !reloadContext(false)) return;
    callHook(LifecycleHook::Ready);
    started_ = true;
}

void ScriptBehaviour::onUpdate(float dt) {
    SAIDA_PROFILE_SCOPE("Scripting/Update");
    {
        SAIDA_PROFILE_SCOPE("Scripting/HotReloadCheck");
        checkHotReload(dt);
    }
    callHook(LifecycleHook::Update, dt);
}

void ScriptBehaviour::onDestroy() {
    callHook(LifecycleHook::Destroy);
    if (context_) cancelJsTimersForContext(context_->raw());
    context_.reset();
    loaded_ = false;
    started_ = false;
    lifecycleHooks_.fill(false);
}

void ScriptBehaviour::onEnable() {
    callHook(LifecycleHook::Enable);
}

void ScriptBehaviour::onDisable() {
    callHook(LifecycleHook::Disable);
}

void ScriptBehaviour::save(nlohmann::json& j) const {
    j["script"] = scriptPath_;
    j["hotReload"] = hotReloadEnabled_;
    nlohmann::json props = nlohmann::json::object();
    for (const auto& property : properties_) {
        switch (property.type) {
        case ScriptProperty::Type::Number:
            props[property.name] = std::get<double>(property.value);
            break;
        case ScriptProperty::Type::Boolean:
            props[property.name] = std::get<bool>(property.value);
            break;
        case ScriptProperty::Type::String:
            props[property.name] = std::get<std::string>(property.value);
            break;
        }
    }
    j["properties"] = std::move(props);
}

void ScriptBehaviour::load(const nlohmann::json& j) {
    scriptPath_ = j.value("script", "");
    hotReloadTimer_ = hotReloadPhase(scriptPath_);
    hotReloadEnabled_ = j.value("hotReload", true);
    properties_.clear();
    if (!j.contains("properties") || !j["properties"].is_object()) return;

    for (auto it = j["properties"].begin(); it != j["properties"].end(); ++it) {
        ScriptProperty property;
        property.name = it.key();
        if (it.value().is_boolean()) {
            property.type = ScriptProperty::Type::Boolean;
            property.value = it.value().get<bool>();
        } else if (it.value().is_number()) {
            property.type = ScriptProperty::Type::Number;
            property.value = it.value().get<double>();
        } else if (it.value().is_string()) {
            property.type = ScriptProperty::Type::String;
            property.value = it.value().get<std::string>();
        } else {
            continue;
        }
        properties_.push_back(std::move(property));
    }
}

void ScriptBehaviour::describe(reflect::TypeBuilder<ScriptBehaviour>& t) {
    t.doc("QuickJS gameplay script attached to a node. The 'script' path, "
          "'hotReload' flag and exported 'properties' are serialized by the "
          "hand-written save()/load(), not through reflected properties.");
}

std::string ScriptBehaviour::resolveScriptPath() const {
    if (scriptPath_.empty()) return {};

    std::filesystem::path p(scriptPath_);
    // The loaded project root comes first: that's where a game's scripts
    // live ("scripts/foo.js" in the .scene is relative to the project).
    if (!activeProjectRoot().empty()) {
        SandboxedPathResult resolved =
            resolveSandboxedProjectPath(activeProjectRoot(), scriptPath_);
        if (!resolved) {
            Log::error("ScriptBehaviour: rejected script path '", scriptPath_,
                       "': ", resolved.error);
            return {};
        }
        if (std::filesystem::is_regular_file(resolved.absolute)) return resolved.absolute;
        Log::error("ScriptBehaviour: project script not found: ", resolved.relative);
        return {};
    }

    // Development tools can validate a standalone script without a project.
    // Shipped games always take the sandboxed branch above.
    if (p.is_absolute() && std::filesystem::exists(p)) return p.string();

    std::filesystem::path cwdRelative = std::filesystem::current_path() / p;
    if (std::filesystem::exists(cwdRelative)) return cwdRelative.string();

    std::filesystem::path rootRelative = assetPath(scriptPath_);
    if (std::filesystem::exists(rootRelative)) return rootRelative.string();

    return p.string();
}

bool ScriptBehaviour::shouldLoadAsModule(const std::string& resolvedPath) const {
    std::filesystem::path path(resolvedPath);
    return path.extension() == ".mjs";
}

void ScriptBehaviour::inspectLifecycleHooks() {
    bool hasCallableHook = false;
    for (std::size_t index = 0; index < kLifecycleHookNames.size(); ++index) {
        const char* name = kLifecycleHookNames[index];
        const JsFunctionStatus status = moduleMode_
            ? context_->moduleExportFunctionStatus(name)
            : context_->globalFunctionStatus(name);
        lifecycleHooks_[index] = status == JsFunctionStatus::Callable;
        hasCallableHook = hasCallableHook || lifecycleHooks_[index];
        if (status == JsFunctionStatus::Invalid)
            Log::warn("ScriptBehaviour: lifecycle hook '", name,
                      "' must be a function in ", scriptPath_);
    }

    if (!hasCallableHook)
        Log::warn("ScriptBehaviour: no recognized lifecycle hook in ", scriptPath_);
}

bool ScriptBehaviour::callHook(LifecycleHook hook) {
    const std::size_t index = static_cast<std::size_t>(hook);
    if (!context_ || !lifecycleHooks_[index]) return true;
    const char* name = kLifecycleHookNames[index];
    return moduleMode_ ? context_->callModuleExport(name) : context_->callGlobal(name);
}

bool ScriptBehaviour::callHook(LifecycleHook hook, double arg) {
    const std::size_t index = static_cast<std::size_t>(hook);
    if (!context_ || !lifecycleHooks_[index]) return true;
    const char* name = kLifecycleHookNames[index];
    return moduleMode_ ? context_->callModuleExport(name, arg) : context_->callGlobal(name, arg);
}

void ScriptBehaviour::updateScriptWatchers(const std::string& resolvedPath, const std::vector<std::string>& moduleDependencies) {
    scriptWatcher_.watch(resolvedPath);
    moduleWatchers_.clear();
    moduleWatchers_.reserve(moduleDependencies.size());
    for (const auto& dependency : moduleDependencies) {
        if (dependency == resolvedPath) continue;
        WatchedFile watcher;
        if (watcher.watch(dependency)) {
            moduleWatchers_.push_back(std::move(watcher));
        }
    }
}

bool ScriptBehaviour::scriptFilesChanged() {
    if (!scriptWatcher_.active()) {
        std::string path = resolveScriptPath();
        if (!path.empty()) scriptWatcher_.watch(path);
        return false;
    }

    if (scriptWatcher_.pollChanged()) return true;
    for (auto& watcher : moduleWatchers_) {
        if (watcher.pollChanged()) return true;
    }
    return false;
}

void ScriptBehaviour::checkHotReload(float dt) {
    if (!hotReloadEnabled_ || scriptPath_.empty()) return;

    hotReloadTimer_ += dt;
    if (hotReloadTimer_ < kHotReloadCheckIntervalSeconds) return;
    hotReloadTimer_ = 0.0f;

    if (scriptFilesChanged()) {
        Log::info("ScriptBehaviour: hot reload ", scriptPath_);
        if (!reload()) {
            Log::warn("ScriptBehaviour: keeping previous script after reload failure");
        }
    }
}

void ScriptBehaviour::installScriptApi() {
    if (!context_) return;

    JSContext* ctx = context_->raw();
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "exportProperty", JS_NewCFunction(ctx, jsExportProperty, "exportProperty", 2));
    JS_FreeValue(ctx, global);
}

void ScriptBehaviour::preparePropertyReload() {
    for (auto& property : properties_) {
        property.exportedThisLoad = false;
    }
}

void ScriptBehaviour::pruneUnexportedProperties() {
    properties_.erase(
        std::remove_if(properties_.begin(), properties_.end(), [](const ScriptProperty& property) {
            return !property.exportedThisLoad;
        }),
        properties_.end());
}

void ScriptBehaviour::applyAllPropertiesToJs() {
    if (!context_) return;
    for (const auto& property : properties_) {
        applyPropertyToJs(property);
    }
}

void ScriptBehaviour::applyPropertyToJs(const ScriptProperty& property) {
    if (!context_) return;

    JSContext* ctx = context_->raw();
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue props = JS_GetPropertyStr(ctx, global, "props");
    if (!JS_IsObject(props)) {
        JS_FreeValue(ctx, props);
        props = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "props", JS_DupValue(ctx, props));
    }

    JS_SetPropertyStr(ctx, props, property.name.c_str(), propertyToJs(ctx, property));
    JS_FreeValue(ctx, props);
    JS_FreeValue(ctx, global);
}

ScriptProperty* ScriptBehaviour::findProperty(const std::string& name) {
    for (auto& property : properties_) {
        if (property.name == name) return &property;
    }
    return nullptr;
}

const ScriptProperty* ScriptBehaviour::findProperty(const std::string& name) const {
    for (const auto& property : properties_) {
        if (property.name == name) return &property;
    }
    return nullptr;
}

void ScriptBehaviour::setPropertyValue(ScriptProperty& property, double value) {
    property.type = ScriptProperty::Type::Number;
    property.value = value;
}

void ScriptBehaviour::setPropertyValue(ScriptProperty& property, bool value) {
    property.type = ScriptProperty::Type::Boolean;
    property.value = value;
}

void ScriptBehaviour::setPropertyValue(ScriptProperty& property, const std::string& value) {
    property.type = ScriptProperty::Type::String;
    property.value = value;
}

} // namespace saida
