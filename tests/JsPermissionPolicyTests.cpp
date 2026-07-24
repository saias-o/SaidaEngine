// P0.4: permission policy for public scripts (capability contract).
//
// A Saida script has NO ambient authority beyond the globals the engine
// explicitly installs: `console` (JsContext) and the gameplay capabilities
// `node/time/input/tree/assets/audio/physics/storage` (JsEngineBindings).
// No network, no OS/process/env access, no filesystem outside `storage`
// (quotas), and imports confined to the project root (proven by
// saida_js_safety_tests), interruptible time budget.
//
// This test locks down the surface: the delta between a bare QuickJS context
// and an engine context must be EXACTLY the allowlist -- any new ambient
// authority (or a disappearance) breaks the test and must go through SPEC.md.
#include "scene/Node.hpp"
#include "scene/ReflectedTypes.hpp"
#include "behaviours/RotatorBehaviour.hpp"
#include "scripting/JsContext.hpp"
#include "scripting/JsEngineBindings.hpp"
#include "scripting/JsRuntime.hpp"

#include <quickjs.h>

#include <cassert>
#include <cstdio>
#include <set>
#include <sstream>
#include <string>

using namespace saida;

namespace {

// Own properties of globalThis, newline-joined, via a raw JS_Eval (works on a
// bare context as well as an engine one).
std::set<std::string> globalNames(JSContext* ctx) {
    const char* code = "Object.getOwnPropertyNames(globalThis).sort().join('\\n')";
    JSValue result = JS_Eval(ctx, code, strlen(code), "<policy>", JS_EVAL_TYPE_GLOBAL);
    assert(!JS_IsException(result));
    const char* text = JS_ToCString(ctx, result);
    assert(text);
    std::set<std::string> names;
    std::stringstream stream((std::string(text)));
    std::string line;
    while (std::getline(stream, line))
        if (!line.empty()) names.insert(line);
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, result);
    return names;
}

} // namespace

int main() {
    registerReflectedTypes();
    JsRuntime& runtime = JsRuntime::instance();

    // Baseline: bare QuickJS context (the language alone, without quickjs-libc).
    JSContext* bare = JS_NewContext(runtime.raw());
    assert(bare);
    const std::set<std::string> baseline = globalNames(bare);

    // The bare language must not already provide any system authority: if a
    // QuickJS update added one of these names to the language, the policy
    // would need to be explicitly re-decided.
    for (const char* forbidden : {"std", "os", "process", "require", "fetch",
                                  "XMLHttpRequest", "WebSocket", "setTimeout",
                                  "setInterval", "open", "exec", "read", "write"}) {
        assert(baseline.find(forbidden) == baseline.end());
    }

    // Full engine context (same bindings as gameplay scripts).
    Node node("Hero");
    auto* rotor = node.addBehaviour<RotatorBehaviour>();
    {
        JsContext ctx(runtime);
        JsEngineBindings::installForBehaviour(ctx, *rotor);
        const std::set<std::string> engine = globalNames(ctx.raw());

        // Exact allowlist: the engine's capabilities, nothing else.
        const std::set<std::string> allowed = {
            "console",  // JsContext
            "node", "time", "input", "tree", "assets", "audio", "physics",
            "storage",  // JsEngineBindings
        };

        std::set<std::string> added;
        for (const std::string& name : engine)
            if (baseline.find(name) == baseline.end()) added.insert(name);

        for (const std::string& name : added) {
            if (allowed.find(name) == allowed.end()) {
                std::printf("FAIL: unexpected ambient global '%s' — update the "
                            "policy in SPEC.md before exposing it\n", name.c_str());
                return 1;
            }
        }
        for (const std::string& name : allowed) {
            if (added.find(name) == added.end()) {
                std::printf("FAIL: expected capability '%s' missing from the "
                            "script surface\n", name.c_str());
                return 1;
            }
        }

        // The classic escape hatches remain absent from the engine context.
        assert(ctx.eval("if (typeof std !== 'undefined') throw new Error('std');"));
        assert(ctx.eval("if (typeof os !== 'undefined') throw new Error('os');"));
        assert(ctx.eval("if (typeof fetch !== 'undefined') throw new Error('fetch');"));
        assert(ctx.eval("if (typeof require !== 'undefined') throw new Error('require');"));
    }

    JS_FreeContext(bare);
    std::printf("PASS: script permission policy (capability surface locked)\n");
    return 0;
}
