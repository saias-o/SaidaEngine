// Locks in M2b: the JS `node.on(signal, fn)` / `node.emit(signal, ...args)`
// bindings bridge reflected C++ signals, and subscriptions are dropped cleanly
// when the JsContext is torn down (the hot-reload lifetime guarantee).

#include "core/Reflection.hpp"
#include "core/Signal.hpp"
#include "scene/Node.hpp"
#include "scene/ReflectedTypes.hpp"
#include "behaviours/RotatorBehaviour.hpp"
#include "scripting/JsContext.hpp"
#include "scripting/JsEngineBindings.hpp"
#include "scripting/JsRuntime.hpp"

#include <quickjs.h>

#include <cassert>

int main() {
    saida::registerReflectedTypes();

    saida::Node node("Player");
    auto* rotor = node.addBehaviour<saida::RotatorBehaviour>();

    // A C++ listener on the same reflected signal, to observe emits from both sides.
    int cppHits = 0;
    auto cppConn = rotor->fullRotation.connect([&] { ++cppHits; });

    saida::JsRuntime& runtime = saida::JsRuntime::instance();

    {
        saida::JsContext ctx(runtime);
        saida::JsEngineBindings::installForBehaviour(ctx, *rotor);

        // JS → C++: node.emit fires the reflected signal (C++ listener sees it).
        assert(ctx.eval("node.emit('fullRotation');"));
        assert(cppHits == 1);

        // C++ → JS: node.on subscribes a JS handler.
        assert(ctx.eval("globalThis.hits = 0; node.on('fullRotation', function(){ globalThis.hits++; });"));
        rotor->fullRotation.emit();  // cppHits=2, JS hits=1
        rotor->fullRotation.emit();  // cppHits=3, JS hits=2
        assert(cppHits == 3);

        // Read the JS-side counter back to confirm the handler actually ran.
        JSContext* raw = ctx.raw();
        JSValue global = JS_GetGlobalObject(raw);
        JSValue hitsVal = JS_GetPropertyStr(raw, global, "hits");
        int32_t hits = -1;
        JS_ToInt32(raw, &hits, hitsVal);
        JS_FreeValue(raw, hitsVal);
        JS_FreeValue(raw, global);
        assert(hits == 2);

        // Unknown signal name returns false (rejected), never throws.
        assert(ctx.eval("if (node.emit('nope') !== false) throw new Error('should reject');"));
    }

    // Context destroyed → the JS subscription is gone. Emitting must be safe and
    // only reach the surviving C++ listener (no dangling call into freed JS).
    rotor->fullRotation.emit();
    assert(cppHits == 4);

    return 0;
}
