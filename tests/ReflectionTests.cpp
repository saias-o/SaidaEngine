// Locks in the M1 reflection foundation: manifest generation, auto save/load
// round-trip, and the named signal/slot descriptors used by the M2 wiring layer.

#include "core/Reflection.hpp"
#include "audio/AudioSourceBehaviour.hpp"
#include "behaviours/CameraFollowBehaviour.hpp"
#include "scene/NodeRegistry.hpp"
#include "nodes/ParticleSystemNode.hpp"
#include "scene/ReflectedTypes.hpp"
#include "behaviours/RotatorBehaviour.hpp"
#include "behaviours/SpawnerBehaviour.hpp"
#include "scene/animation/Timeline.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <string>

using nlohmann::json;

int main() {
    saida::registerReflectedTypes();

    auto& reg = saida::reflect::TypeRegistry::instance();

    // ── manifest: Rotator is present with the expected shape ──
    const saida::reflect::TypeDesc* rotor = reg.find("Rotator");
    assert(rotor && "Rotator must be registered");
    assert(rotor->category == "behaviour");
    assert(rotor->findProperty("axis") && rotor->findProperty("axis")->kind == "vec3");
    assert(rotor->findProperty("speed") && rotor->findProperty("speed")->kind == "float");
    assert(rotor->findProperty("speed")->hasRange);
    assert(rotor->findSignal("fullRotation") && rotor->findSignal("fullRotation")->arity == 0);
    assert(rotor->findSlot("reset"));

    const saida::reflect::TypeDesc* audio = reg.find("AudioSource");
    assert(audio && audio->category == "behaviour");
    assert(audio->findProperty("audioName") &&
           audio->findProperty("audioName")->kind == "string");
    const saida::reflect::TypeDesc* cameraFollow = reg.find("CameraFollow");
    assert(cameraFollow && cameraFollow->category == "behaviour");
    assert(cameraFollow->findProperty("targetGroup"));
    assert(cameraFollow->findProperty("distance") &&
           cameraFollow->findProperty("distance")->hasRange);
    const saida::reflect::TypeDesc* spawner = reg.find("Spawner");
    assert(spawner && spawner->category == "behaviour");
    assert(spawner->findProperty("scenePath") &&
           spawner->findProperty("scenePath")->kind == "asset");

    // Enum property surfaces labels (LightNode.bakeMode / lightType).
    const saida::reflect::TypeDesc* light = reg.find("LightNode");
    if (!light || light->category != "node") return 1;
    const auto* lightType = light->findProperty("lightType");
    if (!lightType || lightType->kind != "enum" || lightType->enumLabels.size() != 3) return 1;

    const saida::reflect::TypeDesc* particles = reg.find("ParticleSystem");
    assert(particles && particles->category == "node");
    assert(particles->findProperty("effectClass") &&
           particles->findProperty("effectClass")->enumLabels.size() == 7);
    assert(particles->findProperty("effectPath") &&
           particles->findProperty("effectPath")->kind == "string");
    assert(particles->findProperty("blendMode") &&
           particles->findProperty("blendMode")->enumLabels.size() == 2);
    assert(particles->findProperty("shape") &&
           particles->findProperty("shape")->enumLabels.size() == 6);
    assert(particles->findProperty("maxParticles") &&
           particles->findProperty("maxParticles")->kind == "int");
    assert(particles->findProperty("drag") &&
           particles->findProperty("drag")->kind == "float");
    assert(particles->findSignal("finished") && particles->findSignal("finished")->arity == 0);
    assert(particles->findSlot("play"));
    assert(particles->findSlot("stop"));
    assert(particles->findSlot("burst"));
    assert(particles->findSlot("applyEffectPreset"));
    assert(particles->findSlot("loadEffect"));
    assert(saida::NodeRegistry::instance().create("ParticleSystem") != nullptr);

    // Manifest JSON is well-formed and category-filterable.
    json full = reg.manifest();
    assert(full.contains("behaviours") && full.contains("nodes"));
    json onlyNodes = reg.manifest("node");
    assert(onlyNodes.contains("nodes") && !onlyNodes.contains("behaviours"));

    // ── auto save/load round-trip via reflection ──
    saida::RotatorBehaviour src;
    src.axis = {0.0f, 0.0f, 1.0f};
    src.speed = 123.5f;
    json saved;
    src.save(saved);
    assert(saved["speed"].get<float>() == 123.5f);
    assert(saved["axis"][2].get<float>() == 1.0f);

    saida::RotatorBehaviour dst;
    dst.load(saved);
    assert(dst.speed == 123.5f);
    assert(dst.axis.z == 1.0f);

    saida::CameraFollowBehaviour follow;
    nlohmann::json followJson;
    cameraFollow->saveTo(&follow, followJson);
    followJson["distance"] = 8.0f;
    followJson["targetGroup"] = "hero";
    cameraFollow->loadFrom(&follow, followJson);
    assert(follow.distance == 8.0f);
    assert(follow.targetGroup == "hero");

    saida::RotatorBehaviour animated;
    saida::TimelinePropertyTrack speedTrack(animated, "speed");
    speedTrack.addKey(0.0f, 10.0f);
    speedTrack.addKey(2.0f, 30.0f);
    speedTrack.evaluate(1.0f);
    assert(animated.speed == 20.0f);
    saida::TimelinePropertyTrack axisTrack(animated, "axis");
    axisTrack.addKey(0.0f, nlohmann::json::array({0.0f, 1.0f, 0.0f}));
    axisTrack.addKey(1.0f, nlohmann::json::array({0.0f, 0.0f, 1.0f}));
    axisTrack.evaluate(0.5f);
    assert(animated.axis.y == 0.5f);
    assert(animated.axis.z == 0.5f);

    // Missing keys keep defaults (defensive load).
    saida::RotatorBehaviour partial;
    partial.load(json{{"speed", 42.0f}});  // no "axis"
    assert(partial.speed == 42.0f);
    assert(partial.axis.y == 1.0f);  // default preserved

    // ── named signal descriptor wires through to the live Signal<> ──
    int pulses = 0;
    {
        auto conn = rotor->findSignal("fullRotation")->connect(
            &src, [&](const json&) { ++pulses; });
        src.fullRotation.emit();
        src.fullRotation.emit();
    }
    assert(pulses == 2);
    src.fullRotation.emit();  // connection dropped → no more pulses
    assert(pulses == 2);

    // ── slot descriptor invokes the method (no node attached: must not crash) ──
    rotor->findSlot("reset")->invoke(&src, json::array());

    saida::ParticleSystemNode ps;
    ps.maxParticles = 777;
    ps.effectPath = "assets/fx/test.saidafx";
    ps.effectClass = saida::ParticleSystemNode::EffectClass::Magic;
    ps.shape = saida::ParticleSystemNode::Shape::Ring;
    ps.noiseStrength = 2.5f;
    json particleSaved;
    particles->saveTo(&ps, particleSaved);
    assert(particleSaved["maxParticles"].get<int>() == 777);
    assert(particleSaved["effectPath"].get<std::string>() == "assets/fx/test.saidafx");
    assert(particleSaved["effectClass"].get<int>() ==
           static_cast<int>(saida::ParticleSystemNode::EffectClass::Magic));
    assert(particleSaved["shape"].get<int>() ==
           static_cast<int>(saida::ParticleSystemNode::Shape::Ring));
    assert(particleSaved["noiseStrength"].get<float>() == 2.5f);

    saida::ParticleSystemNode loadedPs;
    particles->loadFrom(&loadedPs, particleSaved);
    assert(loadedPs.maxParticles == 777);
    assert(loadedPs.effectPath == "assets/fx/test.saidafx");
    assert(loadedPs.effectClass == saida::ParticleSystemNode::EffectClass::Magic);
    assert(loadedPs.shape == saida::ParticleSystemNode::Shape::Ring);
    assert(loadedPs.noiseStrength == 2.5f);

    bool particleFinished = false;
    {
        auto conn = particles->findSignal("finished")->connect(
            &ps, [&](const json&) { particleFinished = true; });
        ps.finished.emit();
    }
    assert(particleFinished);
    particles->findSlot("stop")->invoke(&ps, json::array());
    assert(!ps.playing);
    particles->findSlot("play")->invoke(&ps, json::array());
    assert(ps.playing);
    ps.effectClass = saida::ParticleSystemNode::EffectClass::Explosion;
    particles->findSlot("applyEffectPreset")->invoke(&ps, json::array());
    assert(ps.effectClass == saida::ParticleSystemNode::EffectClass::Explosion);
    assert(ps.spawnRate == 0.0f);
    assert(!ps.looping);

    return 0;
}
