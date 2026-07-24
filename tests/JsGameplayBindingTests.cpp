// P0.4: the gameplay JS bindings -- animation (playClip/currentClip), graph
// (setAnimFloat/Bool/Trigger into the animation blackboard), sequences
// (playSequence/stopSequence + reflected signals), and the gameplay
// Blackboard (setData/getData/hasData + `changed` signal) -- exercised from a
// real headless QuickJS context. Targets without the behaviour respond with
// false/null, never an exception.
#include "core/Reflection.hpp"
#include "core/Input.hpp"
#include "scene/Blackboard.hpp"
#include "scene/Node.hpp"
#include "scene/ReflectedTypes.hpp"
#include "behaviours/RotatorBehaviour.hpp"
#include "scene/animation/AnimationClip.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/Rig.hpp"
#include "scene/animation/SequenceDirectorBehaviour.hpp"
#include "scripting/JsContext.hpp"
#include "scripting/JsEngineBindings.hpp"
#include "scripting/JsRuntime.hpp"

#include <quickjs.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace saida;

namespace {

Rig makeRig() {
    Rig rig;
    Transform root;
    rig.addBone("root", -1, glm::mat4(1.0f), root);
    std::string error;
    const bool ok = rig.finalize(&error);
    assert(ok);
    (void)ok;
    return rig;
}

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

    // Carrier node: Animator (rig with 1 bone + "Idle" clip), SequenceDirector
    // (autoplay off, sequence absent -- only the play/stop binding is tested
    // here, the real .sseq traversal lives in WitnessGame), Blackboard.
    Node node("Hero");
    Rig rig = makeRig();
    auto* animator = node.addBehaviour<Animator>();
    animator->setRig(&rig);
    AnimationClip idle("Idle", 1.0f);
    animator->addClip("Idle", &idle);
    auto* director = node.addBehaviour<SequenceDirectorBehaviour>();
    director->autoplay = false;
    auto* board = node.addBehaviour<Blackboard>();

    // A node with no gameplay behaviours, for the negative responses.
    Node bare("Empty");
    auto* bareRotor = bare.addBehaviour<RotatorBehaviour>();

    {
        JsContext ctx(runtime);
        JsEngineBindings::installForBehaviour(ctx, *animator);

        // --- animation ---
        assert(ctx.eval("if (node.playClip('Idle') !== true) throw new Error('playClip');"));
        assert(animator->currentClip() == "Idle");
        assert(ctx.eval("if (node.currentClip() !== 'Idle') throw new Error('currentClip');"));
        // Unknown clip: the Animator is found (true) and logs the engine warning.
        assert(ctx.eval("if (node.playClip('Nope') !== true) throw new Error('unknown clip');"));
        assert(animator->currentClip() == "Idle");

        // --- graph / blackboard d'animation ---
        assert(ctx.eval("if (node.setAnimFloat('speed', 2.5) !== true) throw new Error('setAnimFloat');"));
        assert(std::fabs(animator->blackboard().getFloat("speed") - 2.5f) < 1e-6f);
        assert(ctx.eval("if (node.setAnimBool('armed', true) !== true) throw new Error('setAnimBool');"));
        assert(animator->blackboard().getBool("armed"));
        assert(ctx.eval("if (node.setAnimTrigger('fire') !== true) throw new Error('setAnimTrigger');"));
        assert(animator->blackboard().getFloat("fire") == 1.0f);

        // --- reflected animationEvent signal (C++ -> JS) ---
        assert(ctx.eval("globalThis.animEvents = 0;"
                        "if (node.on('animationEvent', function(n){"
                        "  if (n === 'step') globalThis.animEvents++; }) !== true)"
                        "  throw new Error('on(animationEvent)');"));
        animator->animationEvent.emit("step");
        assert(readGlobalInt(ctx, "animEvents") == 1);

        // --- sequences ---
        assert(ctx.eval("if (node.playSequence() !== true) throw new Error('playSequence');"));
        assert(ctx.eval("if (node.stopSequence() !== true) throw new Error('stopSequence');"));

        // --- Blackboard gameplay + signal changed ---
        assert(ctx.eval("globalThis.changes = [];"
                        "node.on('changed', function(k){ globalThis.changes.push(k); });"));
        assert(ctx.eval("if (node.setData('score', 42) !== true) throw new Error('setData num');"));
        assert(ctx.eval("if (node.setData('alive', true) !== true) throw new Error('setData bool');"));
        assert(ctx.eval("if (node.setData('zone', 'hub') !== true) throw new Error('setData str');"));
        assert(board->number("score") == 42.0);
        assert(board->boolean("alive"));
        assert(board->string("zone") == "hub");
        assert(ctx.eval("if (node.getData('score') !== 42) throw new Error('getData num');"));
        assert(ctx.eval("if (node.getData('alive') !== true) throw new Error('getData bool');"));
        assert(ctx.eval("if (node.getData('zone') !== 'hub') throw new Error('getData str');"));
        assert(ctx.eval("if (node.getData('missing') !== null) throw new Error('getData null');"));
        assert(ctx.eval("if (node.getData('missing', 7) !== 7) throw new Error('getData fallback');"));
        assert(ctx.eval("if (node.hasData('score') !== true) throw new Error('hasData');"));
        assert(ctx.eval("if (node.hasData('missing') !== false) throw new Error('hasData missing');"));
        assert(ctx.eval("if (globalThis.changes.length !== 3) throw new Error('changed count');"));
        // C++ -> C++ via the same store: the value written on the C++ side is visible in JS.
        board->setNumber("score", 43.0);
        assert(ctx.eval("if (node.getData('score') !== 43) throw new Error('C++ write visible');"));

        // --- rebinding runtime + profil JSON persistable via storage.prefs ---
        assert(ctx.eval(
            "input.rebindKey('Jump', 'J');"
            "input.rebindMouse('Fire', 'Button4', 'Gameplay');"
            "input.rebindGamepadButton('Confirm', 'A');"
            "input.rebindGamepadAxis('MoveLeft', 'LeftX', -1.0, 0.2);"
            "input.rebindTouch('Dash', 'SwipeRight', 0.5, 0, 1, 1, 64);"
            "globalThis.inputProfile = input.exportProfile('custom');"
            "const p = JSON.parse(globalThis.inputProfile);"
            "if (p.schema !== 1 || p.name !== 'custom' || p.bindings.length !== 5)"
            "  throw new Error('profile export');"
            "if (input.applyProfile(globalThis.inputProfile) !== true)"
            "  throw new Error('profile apply');"));
        const std::string profileBeforeInvalid =
            Input::serializeBindingProfile("custom");
        std::string profileError;
        assert(!Input::applyBindingProfile(
            R"({"schema":99,"name":"future","bindings":[]})", profileError));
        assert(!profileError.empty());
        assert(Input::serializeBindingProfile("custom") == profileBeforeInvalid);
        assert(ctx.eval(
            "let rejected = false;"
            "try { input.applyProfile('{\"schema\":99,\"name\":\"bad\",\"bindings\":[]}'); }"
            "catch (e) { rejected = true; }"
            "if (!rejected) throw new Error('invalid profile accepted');"));
        assert(ctx.eval(
            "if (input.lastActiveDevice() !== 'none')"
            "  throw new Error('last active device default');"));
        assert(ctx.eval(
            "if (input.rumble(0.25, 0.75, 100) !== false)"
            "  throw new Error('headless rumble must report unavailable');"
            "if (input.stopRumble() !== false)"
            "  throw new Error('headless stopRumble must report unavailable');"));
    }

    {
        // Target with no gameplay behaviours: everything responds false/null, without throwing.
        JsContext ctx(runtime);
        JsEngineBindings::installForBehaviour(ctx, *bareRotor);
        assert(ctx.eval("if (node.playClip('Idle') !== false) throw new Error('bare playClip');"));
        assert(ctx.eval("if (node.currentClip() !== null) throw new Error('bare currentClip');"));
        assert(ctx.eval("if (node.setAnimFloat('x', 1) !== false) throw new Error('bare setAnimFloat');"));
        assert(ctx.eval("if (node.playSequence() !== false) throw new Error('bare playSequence');"));
        assert(ctx.eval("if (node.setData('k', 1) !== false) throw new Error('bare setData');"));
        assert(ctx.eval("if (node.getData('k') !== null) throw new Error('bare getData');"));
        assert(ctx.eval("if (node.hasData('k') !== false) throw new Error('bare hasData');"));
    }

    std::printf("PASS: JS gameplay bindings (animation/graph/sequence/blackboard)\n");
    return 0;
}
