#pragma once

#include "core/FileWatcher.hpp"
#include "core/ReflectionFwd.hpp"
#include "scene/Behaviour.hpp"
#include "scripting/JsContext.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace saida {

class JsTimerBindings;

struct ScriptProperty {
    enum class Type {
        Number,
        Boolean,
        String
    };

    std::string name;
    Type type = Type::Number;
    std::variant<double, bool, std::string> value = 0.0;
    bool exportedThisLoad = false;
};

enum class ScriptCallStatus {
    Missing,
    Succeeded,
    Failed
};

class ScriptBehaviour : public Behaviour {
public:
    ~ScriptBehaviour() override;

    void setScriptPath(std::string path);
    const std::string& scriptPath() const { return scriptPath_; }

    bool reload();
    void exportNumberProperty(const std::string& name, double defaultValue);
    void exportBoolProperty(const std::string& name, bool defaultValue);
    void exportStringProperty(const std::string& name, const std::string& defaultValue);

    // Editor-facing accessors (the inspector UI lives in the editor's
    // InspectorRegistry, not in this gameplay behaviour).
    bool loaded() const { return loaded_; }
    bool hotReloadEnabled() const { return hotReloadEnabled_; }
    void setHotReloadEnabled(bool enabled) { hotReloadEnabled_ = enabled; }
    std::vector<ScriptProperty>& properties() { return properties_; }
    void applyProperty(const ScriptProperty& property) { applyPropertyToJs(property); }

    // Invoke a JSON-compatible function exported by this script. This is the
    // cross-context bridge used by NodeRef.call(); JS values never leak between
    // QuickJS contexts.
    ScriptCallStatus callExport(const std::string& name,
                                const nlohmann::json& args,
                                nlohmann::json& result);

    void onReady() override;
    void onUpdate(float dt) override;
    void onDestroy() override;
    void onEnable() override;
    void onDisable() override;

    const char* typeName() const override { return "ScriptBehaviour"; }
    void save(nlohmann::json& j) const override;
    void load(const nlohmann::json& j) override;

    // Reflection: descriptor without properties — serialization stays the
    // hand-written save()/load() (the `script`/`hotReload`/`properties`
    // payload is a durable format). Registration flows through
    // registerReflectedTypes() like every other built-in behaviour.
    static constexpr const char* reflectName() { return "ScriptBehaviour"; }
    static void describe(reflect::TypeBuilder<ScriptBehaviour>& t);

private:
    friend class JsTimerBindings;

    enum class LifecycleHook : std::size_t {
        Ready,
        Update,
        Destroy,
        Enable,
        Disable,
        Count
    };

    static constexpr std::array<const char*, static_cast<std::size_t>(LifecycleHook::Count)>
        kLifecycleHookNames{"onReady", "onUpdate", "onDestroy", "onEnable", "onDisable"};

    enum class JsTimerKind { Wait, Every, Tween };

    struct JsTimerCallback {
        uint64_t callbackId = 0;
        uint64_t timerId = 0;
        uint64_t cleanupTimerId = 0;
        JSContext* context = nullptr;
        JSValue callback = JS_UNDEFINED;
        JsTimerKind kind = JsTimerKind::Wait;
    };

    uint64_t scheduleJsTimer(JSContext* context, JSValueConst callback,
                             JsTimerKind kind, float duration, Easing easing);
    bool cancelJsTimer(uint64_t timerId);
    void cancelJsTimersForContext(JSContext* context);
    void cancelAllJsTimers();
    void invokeJsTimer(uint64_t callbackId, float tweenValue = 0.0f);
    void removeJsTimer(uint64_t callbackId, bool cancelEngineTimer);

    std::string resolveScriptPath() const;
    bool reloadContext(bool lifecycleReload);
    bool shouldLoadAsModule(const std::string& resolvedPath) const;
    void inspectLifecycleHooks();
    bool callHook(LifecycleHook hook);
    bool callHook(LifecycleHook hook, double arg);
    void updateScriptWatchers(const std::string& resolvedPath, const std::vector<std::string>& moduleDependencies);
    bool scriptFilesChanged();
    void checkHotReload(float dt);
    void installScriptApi();
    void preparePropertyReload();
    void pruneUnexportedProperties();
    void applyAllPropertiesToJs();
    void applyPropertyToJs(const ScriptProperty& property);
    ScriptProperty* findProperty(const std::string& name);
    const ScriptProperty* findProperty(const std::string& name) const;
    void setPropertyValue(ScriptProperty& property, double value);
    void setPropertyValue(ScriptProperty& property, bool value);
    void setPropertyValue(ScriptProperty& property, const std::string& value);

    std::string scriptPath_;
    WatchedFile scriptWatcher_;
    std::vector<WatchedFile> moduleWatchers_;
    std::vector<ScriptProperty> properties_;
    std::unique_ptr<JsContext> context_;
    std::vector<JsTimerCallback> jsTimers_;
    uint64_t nextJsCallbackId_ = 0;
    float hotReloadTimer_ = 0.0f;
    bool hotReloadEnabled_ = true;
    bool started_ = false;
    bool moduleMode_ = false;
    bool loaded_ = false;
    std::array<bool, kLifecycleHookNames.size()> lifecycleHooks_{};
};

} // namespace saida
