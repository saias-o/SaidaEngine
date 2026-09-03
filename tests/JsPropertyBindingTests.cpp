// The reflected-property half of the JS reflection bridge:
// `node.getProperty(name)` / `node.setProperty(name, value)`.
//
// Two things are pinned here. First, the resolution order — the node's own
// reflected type, then its behaviours — which is the order SequenceDirector's
// property binder and the gameplay bindings already use; a script must not
// reach a behaviour's property while shadowing the node's own. Second, that a
// write of the wrong shape is REFUSED rather than absorbed: the setters built
// by TypeBuilder are `Traits<M>::from`, which leave the property untouched when
// the JSON does not match, so without the kind check a script would be told a
// value it never applied had been applied.
//
// Every kind the reflection defines is covered, at the level it exists at: the
// four carried by a registered type through the live JS binding, and all eight
// through the shared validator — no registered type declares a `quat` property
// today, and the validator must still be right about it the day one does.
#include "core/Reflection.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/ParticleSystemNode.hpp"
#include "scene/Node.hpp"
#include "scene/ReflectedTypes.hpp"
#include "behaviours/RotatorBehaviour.hpp"
#include "scripting/JsContext.hpp"
#include "scripting/JsEngineBindings.hpp"
#include "scripting/JsRuntime.hpp"

#include <quickjs.h>

#include <cassert>
#include <cmath>
#include <string>

using namespace saida;

namespace {

bool nearly(float a, float b) { return std::fabs(a - b) < 1e-4f; }

void checkKind(const char* kindName, const nlohmann::json& accepted,
               const nlohmann::json& rejected) {
    reflect::PropertyDesc prop;
    prop.kind = kindName;
    std::string why;
    assert(reflect::valueMatchesKind(prop, accepted, why));
    assert(!reflect::valueMatchesKind(prop, rejected, why));
    assert(!why.empty());
}

} // namespace

int main() {
    registerReflectedTypes();
    JsRuntime& runtime = JsRuntime::instance();

    // One carrier proves both halves of the resolution order at once: the
    // script's node is a LightNode (own reflected properties) and it carries a
    // Rotator (behaviour properties).
    LightNode light("Sun");
    light.type = LightType::Directional;
    light.color = {1.0f, 1.0f, 1.0f};
    light.intensity = 1.0f;
    light.castShadows = true;
    auto* rotor = light.addBehaviour<RotatorBehaviour>();
    rotor->speed = 10.0f;

    {
        JsContext ctx(runtime);
        JsEngineBindings::installForBehaviour(ctx, *rotor);

        // float / vec3 / bool / enum on the node's own type.
        assert(ctx.eval("if (node.getProperty('intensity') !== 1) throw new Error('float read');"));
        assert(ctx.eval("if (node.setProperty('intensity', 4.25) !== true) throw new Error('float write');"));
        assert(nearly(light.intensity, 4.25f));

        assert(ctx.eval(
            "const c = node.getProperty('color');"
            "if (!Array.isArray(c) || c.length !== 3 || c[0] !== 1) throw new Error('vec3 read');"
            "if (node.setProperty('color', [0.25, 0.5, 0.75]) !== true) throw new Error('vec3 write');"));
        assert(nearly(light.color.r, 0.25f) && nearly(light.color.g, 0.5f) && nearly(light.color.b, 0.75f));

        assert(ctx.eval("if (node.getProperty('castShadows') !== true) throw new Error('bool read');"
                        "if (node.setProperty('castShadows', false) !== true) throw new Error('bool write');"));
        assert(!light.castShadows);

        assert(ctx.eval("if (node.getProperty('lightType') !== 0) throw new Error('enum read');"
                        "if (node.setProperty('lightType', 2) !== true) throw new Error('enum write');"));
        assert(light.type == LightType::Spot);

        // The behaviour is reached only because the node itself has no such
        // property: same rule as the signal bridge next to it.
        assert(ctx.eval("if (node.getProperty('speed') !== 10) throw new Error('behaviour read');"
                        "if (node.setProperty('speed', -30) !== true) throw new Error('behaviour write');"));
        assert(nearly(rotor->speed, -30.0f));

        // A wrong shape is refused and the value is left alone. Without the kind
        // check `Traits<vec3>::from` would ignore the number and the call would
        // still report success.
        assert(ctx.eval("if (node.setProperty('color', 0.5) !== false) throw new Error('vec3 kind');"));
        assert(nearly(light.color.r, 0.25f) && nearly(light.color.g, 0.5f) && nearly(light.color.b, 0.75f));

        assert(ctx.eval("if (node.setProperty('castShadows', 1) !== false) throw new Error('bool kind');"));
        assert(!light.castShadows);

        assert(ctx.eval("if (node.setProperty('color', [1, 2]) !== false) throw new Error('vec3 arity');"));
        assert(nearly(light.color.r, 0.25f));

        // An enum outside its declared labels is out of range, not a new state.
        assert(ctx.eval("if (node.setProperty('lightType', 7) !== false) throw new Error('enum range');"));
        assert(light.type == LightType::Spot);

        // An unknown property answers null / false, never an exception: asking a
        // node whether it carries one is a legitimate probe.
        assert(ctx.eval("if (node.getProperty('nope') !== null) throw new Error('unknown read');"
                        "if (node.setProperty('nope', 1) !== false) throw new Error('unknown write');"));

        // Missing arguments are refused the same way rather than throwing.
        assert(ctx.eval("if (node.getProperty() !== null) throw new Error('no name read');"
                        "if (node.setProperty('intensity') !== false) throw new Error('no value');"));

        // Range metadata is an inspector hint, not a clamp — no writer enforces
        // it, and this binding must not silently become the one that does.
        assert(ctx.eval("if (node.setProperty('intensity', 5000) !== true) throw new Error('range');"));
        assert(nearly(light.intensity, 5000.0f));
    }

    // int / string / vec4 exist on a different registered type; the binding is
    // the same one, so they are checked where they live.
    {
        ParticleSystemNode particles;
        auto* rotor2 = particles.addBehaviour<RotatorBehaviour>();
        JsContext ctx(runtime);
        JsEngineBindings::installForBehaviour(ctx, *rotor2);

        assert(ctx.eval("if (node.setProperty('maxParticles', 512) !== true) throw new Error('int write');"
                        "if (node.getProperty('maxParticles') !== 512) throw new Error('int read');"));
        assert(particles.maxParticles == 512);

        // An integer property refuses a fractional number rather than truncating
        // it: a silently rounded budget is a defect nobody can see.
        assert(ctx.eval("if (node.setProperty('maxParticles', 12.5) !== false) throw new Error('int kind');"));
        assert(particles.maxParticles == 512);

        assert(ctx.eval("if (node.setProperty('effectPath', 'assets/fx/fire.saidafx') !== true)"
                        "  throw new Error('string write');"
                        "if (node.getProperty('effectPath') !== 'assets/fx/fire.saidafx')"
                        "  throw new Error('string read');"));
        assert(particles.effectPath == "assets/fx/fire.saidafx");

        assert(ctx.eval("if (node.setProperty('effectPath', 3) !== false) throw new Error('string kind');"));
        assert(particles.effectPath == "assets/fx/fire.saidafx");

        assert(ctx.eval("if (node.setProperty('startColor', [1, 0.5, 0.25, 0.125]) !== true)"
                        "  throw new Error('vec4 write');"
                        "const s = node.getProperty('startColor');"
                        "if (s.length !== 4 || s[3] !== 0.125) throw new Error('vec4 read');"));
        assert(nearly(particles.startColor.a, 0.125f));

        assert(ctx.eval("if (node.setProperty('startColor', [1, 0, 0]) !== false) throw new Error('vec4 arity');"));
        assert(nearly(particles.startColor.a, 0.125f));
    }

    // The validator itself, over every kind the reflection defines. `quat` has
    // no registered property to exercise it through the binding yet.
    checkKind("bool", true, 1);
    checkKind("float", 1.5, "1.5");
    checkKind("int", 3, 3.5);
    checkKind("string", "value", 3);
    checkKind("asset", "assets/fx/fire.saidafx", nullptr);
    checkKind("vec3", nlohmann::json::array({1, 2, 3}), nlohmann::json::array({1, 2}));
    checkKind("vec4", nlohmann::json::array({1, 2, 3, 4}), nlohmann::json::array({1, 2, 3}));
    checkKind("quat", nlohmann::json::array({0, 0, 0, 1}), nlohmann::json::array({0, 0, 1}));

    // An enum with declared labels is bounded by them; one without is not,
    // because the label list is what defines the range.
    {
        reflect::PropertyDesc bounded;
        bounded.kind = "enum";
        bounded.enumLabels = {"Directional", "Point", "Spot"};
        std::string why;
        assert(reflect::valueMatchesKind(bounded, 2, why));
        assert(!reflect::valueMatchesKind(bounded, 3, why));
        assert(!reflect::valueMatchesKind(bounded, -1, why));
        assert(!reflect::valueMatchesKind(bounded, "Spot", why));

        reflect::PropertyDesc open;
        open.kind = "enum";
        assert(reflect::valueMatchesKind(open, 99, why));
    }

    // A kind the validator does not know is not rejected: the reflection's
    // "json" catch-all exists for values it cannot describe, and refusing them
    // here would make a property unwritable rather than unchecked.
    {
        reflect::PropertyDesc opaque;
        opaque.kind = "json";
        std::string why;
        assert(reflect::valueMatchesKind(opaque, nlohmann::json::object(), why));
    }

    return 0;
}
