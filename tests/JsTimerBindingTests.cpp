#include "scripting/JsContext.hpp"
#include "scripting/JsEngineBindings.hpp"
#include "scripting/JsRuntime.hpp"
#include "scripting/ScriptBehaviour.hpp"

#include <cassert>

int main() {
    saida::ScriptBehaviour script;
    saida::JsContext context(saida::JsRuntime::instance());
    saida::JsEngineBindings::installForBehaviour(context, script);

    // The timer surface is installed on the existing time service and rejects
    // malformed requests synchronously, before touching the scene scheduler.
    assert(context.eval(R"JS(
        if (typeof time.wait !== 'function' ||
            typeof time.every !== 'function' ||
            typeof time.tween !== 'function' ||
            typeof time.cancel !== 'function' ||
            typeof assets.load !== 'function' ||
            typeof storage.save !== 'function' ||
            typeof storage.load !== 'function' ||
            typeof storage.has !== 'function' ||
            typeof storage.remove !== 'function') {
            throw new Error('timer API missing');
        }

        function expectThrow(fn) {
            let threw = false;
            try { fn(); } catch (_) { threw = true; }
            if (!threw) throw new Error('expected timer validation error');
        }

        expectThrow(() => time.wait(-1, function() {}));
        expectThrow(() => time.every(0, function() {}));
        expectThrow(() => time.tween(1, function() {}, 'unknown'));
        expectThrow(() => time.wait(1, 42));
        expectThrow(() => time.cancel(0));
    )JS"));

    // A valid request still fails explicitly when the behaviour is not mounted
    // in a SceneTree; it never returns a plausible but inactive timer id.
    assert(context.eval(R"JS(
        let mountedError = false;
        try { time.wait(0, function() {}); }
        catch (error) { mountedError = String(error).includes('SceneTree'); }
        if (!mountedError) throw new Error('missing SceneTree diagnostic');

        let assetError = false;
        try { assets.load('assets/example.bin'); }
        catch (error) { assetError = String(error).includes('SceneTree'); }
        if (!assetError) throw new Error('missing asset SceneTree diagnostic');

        let storageError = false;
        try { storage.save('slot', '{}'); }
        catch (error) { storageError = String(error).includes('SceneTree'); }
        if (!storageError) throw new Error('missing storage SceneTree diagnostic');
    )JS"));

    return 0;
}
