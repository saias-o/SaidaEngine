// P0.4: asynchronous contract of storage. `storage.flush()` returns a
// Promise that resolves to `true` once pending writes are durable (never
// rejects). On desktop, writes are already durable at save() time, so the
// promise resolves on the next microtask drain. On web, resolution comes
// from the FS.syncfs callback -- same contract, proven by WitnessGame's
// browser harness (PASS emitted after flush, save re-read on reload).
#include "scene/Node.hpp"
#include "scene/ReflectedTypes.hpp"
#include "behaviours/RotatorBehaviour.hpp"
#include "scripting/JsContext.hpp"
#include "scripting/JsEngineBindings.hpp"
#include "scripting/JsRuntime.hpp"

#include <quickjs.h>

#include <cassert>
#include <cstdio>

using namespace saida;

namespace {

int readGlobalInt(JsContext& ctx, const char* name) {
    JSContext* raw = ctx.raw();
    JSValue global = JS_GetGlobalObject(raw);
    JSValue v = JS_GetPropertyStr(raw, global, name);
    int32_t out = -1;
    JS_ToInt32(raw, &out, v);
    JS_FreeValue(raw, v);
    JS_FreeValue(raw, global);
    return out;
}

} // namespace

int main() {
    registerReflectedTypes();
    JsRuntime& runtime = JsRuntime::instance();

    Node node("Hero");
    auto* rotor = node.addBehaviour<RotatorBehaviour>();

    JsContext ctx(runtime);
    JsEngineBindings::installForBehaviour(ctx, *rotor);

    // flush() returns a real Promise; the .then reaction runs after the
    // microtask drain, with `true` as the resolution value.
    assert(ctx.eval(
        "globalThis.state = 0;"
        "const p = storage.flush();"
        "if (!(p instanceof Promise)) throw new Error('flush must return a Promise');"
        "p.then(function(ok) { globalThis.state = ok === true ? 1 : 2; });"));
    // ctx.eval already drains jobs on exit; an explicit drain remains safe.
    assert(ctx.executePendingJobs());
    assert(readGlobalInt(ctx, "state") == 1);

    // Two in-flight flushes resolve independently.
    assert(ctx.eval(
        "globalThis.count = 0;"
        "storage.flush().then(function() { globalThis.count++; });"
        "storage.flush().then(function() { globalThis.count++; });"));
    assert(ctx.executePendingJobs());
    assert(readGlobalInt(ctx, "count") == 2);

    std::printf("PASS: storage.flush async durability contract\n");
    return 0;
}
