#pragma once

#include <quickjs.h>

namespace saida {

class Behaviour;
class JsContext;
class ScriptBehaviour;

// Installs the asynchronous gameplay timer surface on the existing `time`
// object. ScriptBehaviour owns every callback and engine timer it creates.
class JsTimerBindings {
public:
    static void install(JsContext& context, Behaviour& behaviour,
                        JSValueConst timeObject);

private:
    enum class Mode { Wait, Every, Tween };

    static JSValue wait(JSContext* context, JSValueConst, int argc,
                        JSValueConst* argv);
    static JSValue every(JSContext* context, JSValueConst, int argc,
                         JSValueConst* argv);
    static JSValue tween(JSContext* context, JSValueConst, int argc,
                         JSValueConst* argv);
    static JSValue cancel(JSContext* context, JSValueConst, int argc,
                          JSValueConst* argv);
    static JSValue schedule(JSContext* context, int argc, JSValueConst* argv,
                            Mode mode);
    static ScriptBehaviour* script(JSContext* context);
};

} // namespace saida
