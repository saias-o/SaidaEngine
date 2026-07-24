#include "scripting/JsTimerBindings.hpp"

#include "core/Easing.hpp"
#include "scene/Behaviour.hpp"
#include "scene/SceneTimerQueue.hpp"
#include "scripting/JsContext.hpp"
#include "scripting/ScriptBehaviour.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace saida {

namespace {

std::optional<Easing> parseEasing(JSContext* context, JSValueConst value) {
    if (JS_IsUndefined(value)) return Easing::Linear;
    const char* text = JS_ToCString(context, value);
    if (!text) return std::nullopt;
    const std::string name(text);
    JS_FreeCString(context, text);

    if (name == "linear") return Easing::Linear;
    if (name == "inQuad") return Easing::InQuad;
    if (name == "outQuad") return Easing::OutQuad;
    if (name == "inOutQuad") return Easing::InOutQuad;
    if (name == "outBack") return Easing::OutBack;
    return std::nullopt;
}

} // namespace

ScriptBehaviour* JsTimerBindings::script(JSContext* context) {
    return dynamic_cast<ScriptBehaviour*>(
        static_cast<Behaviour*>(JS_GetContextOpaque(context)));
}

JSValue JsTimerBindings::schedule(JSContext* context, int argc,
                                  JSValueConst* argv, Mode mode) {
    ScriptBehaviour* owner = script(context);
    if (!owner)
        return JS_ThrowInternalError(context,
                                     "time timers require a ScriptBehaviour");
    if (argc < 2 || !JS_IsFunction(context, argv[1]))
        return JS_ThrowTypeError(context,
                                 "expected (durationSeconds, callback)");

    double duration = 0.0;
    if (JS_ToFloat64(context, &duration, argv[0]) < 0 || !std::isfinite(duration))
        return JS_ThrowTypeError(context, "duration must be a finite number");
    if (duration > static_cast<double>(std::numeric_limits<float>::max()))
        return JS_ThrowRangeError(context, "duration is too large");
    if (duration < 0.0 || (mode == Mode::Every && duration == 0.0))
        return JS_ThrowRangeError(context,
                                  mode == Mode::Every
                                      ? "repeat interval must be greater than zero"
                                      : "duration must not be negative");

    Easing easing = Easing::Linear;
    if (mode == Mode::Tween) {
        auto parsed = parseEasing(context, argc >= 3 ? argv[2] : JS_UNDEFINED);
        if (!parsed)
            return JS_ThrowRangeError(
                context,
                "unknown easing; expected linear, inQuad, outQuad, inOutQuad or outBack");
        easing = *parsed;
    }

    ScriptBehaviour::JsTimerKind kind = ScriptBehaviour::JsTimerKind::Wait;
    if (mode == Mode::Every) kind = ScriptBehaviour::JsTimerKind::Every;
    if (mode == Mode::Tween) kind = ScriptBehaviour::JsTimerKind::Tween;

    const uint64_t id = owner->scheduleJsTimer(
        context, argv[1], kind, static_cast<float>(duration), easing);
    if (id == kInvalidTimerId)
        return JS_ThrowInternalError(context,
                                     "timer requires a mounted SceneTree");
    return JS_NewInt64(context, static_cast<int64_t>(id));
}

JSValue JsTimerBindings::wait(JSContext* context, JSValueConst, int argc,
                              JSValueConst* argv) {
    return schedule(context, argc, argv, Mode::Wait);
}

JSValue JsTimerBindings::every(JSContext* context, JSValueConst, int argc,
                               JSValueConst* argv) {
    return schedule(context, argc, argv, Mode::Every);
}

JSValue JsTimerBindings::tween(JSContext* context, JSValueConst, int argc,
                               JSValueConst* argv) {
    return schedule(context, argc, argv, Mode::Tween);
}

JSValue JsTimerBindings::cancel(JSContext* context, JSValueConst, int argc,
                                JSValueConst* argv) {
    ScriptBehaviour* owner = script(context);
    if (!owner)
        return JS_ThrowInternalError(context,
                                     "time timers require a ScriptBehaviour");
    int64_t id = 0;
    if (argc < 1 || JS_ToInt64(context, &id, argv[0]) < 0 || id <= 0)
        return JS_ThrowTypeError(context, "timer id must be a positive integer");
    return JS_NewBool(context, owner->cancelJsTimer(static_cast<uint64_t>(id)));
}

void JsTimerBindings::install(JsContext& context, Behaviour&, JSValueConst timeObject) {
    JSContext* raw = context.raw();
    JS_SetPropertyStr(raw, timeObject, "wait",
                      JS_NewCFunction(raw, wait, "wait", 2));
    JS_SetPropertyStr(raw, timeObject, "every",
                      JS_NewCFunction(raw, every, "every", 2));
    JS_SetPropertyStr(raw, timeObject, "tween",
                      JS_NewCFunction(raw, tween, "tween", 3));
    JS_SetPropertyStr(raw, timeObject, "cancel",
                      JS_NewCFunction(raw, cancel, "cancel", 1));
}

} // namespace saida
