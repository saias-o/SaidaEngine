#include "nodes/ParticleSystemNode.hpp"

#include "core/Log.hpp"
#include "fx/ParticleEffect.hpp"
#include "fx/ParticlePresetLibrary.hpp"

namespace saida {

void ParticleSystemNode::describe(reflect::TypeBuilder<ParticleSystemNode>& t) {
    t.doc("Lightweight billboard particle emitter rendered by SaidaFX.");
    t.property("effectClass", &ParticleSystemNode::effectClass)
        .enumValues({"Simple", "Fire", "Magic", "Rain", "Snow", "Smoke", "Explosion"})
        .tooltip("high-level preset family");
    t.property("effectPath", &ParticleSystemNode::effectPath)
        .tooltip("absolute or project-resolved .saidafx path to load through loadEffect");
    t.property("maxParticles", &ParticleSystemNode::maxParticles).range(1.0, 20000.0)
        .tooltip("maximum live particles for this emitter");
    t.property("spawnRate", &ParticleSystemNode::spawnRate).range(0.0, 5000.0)
        .tooltip("particles spawned per second while playing");
    t.property("lifetime", &ParticleSystemNode::lifetime).range(0.02, 60.0)
        .tooltip("particle lifetime in seconds");
    t.property("startSpeed", &ParticleSystemNode::startSpeed).range(0.0, 100.0)
        .tooltip("initial radial speed");
    t.property("startSize", &ParticleSystemNode::startSize).range(0.001, 100.0)
        .tooltip("initial billboard size in metres");
    t.property("startColor", &ParticleSystemNode::startColor).tooltip("linear HDR start color");
    t.property("endColor", &ParticleSystemNode::endColor).tooltip("linear HDR end color");
    t.property("gravity", &ParticleSystemNode::gravity).tooltip("world-space acceleration");
    t.property("radius", &ParticleSystemNode::radius).range(0.0, 1000.0)
        .tooltip("primary shape radius");
    t.property("shape", &ParticleSystemNode::shape)
        .enumValues({"Point", "Sphere", "Disc", "Box", "Cone", "Ring"})
        .tooltip("emission shape");
    t.property("boxExtents", &ParticleSystemNode::boxExtents)
        .tooltip("box emission half-extents");
    t.property("coneAngle", &ParticleSystemNode::coneAngle).range(1.0, 89.0)
        .tooltip("cone half-angle in degrees");
    t.property("ringThickness", &ParticleSystemNode::ringThickness).range(0.0, 100.0)
        .tooltip("ring radial jitter");
    t.property("emissive", &ParticleSystemNode::emissive).range(0.0, 100.0)
        .tooltip("HDR intensity multiplier");
    t.property("endSizeScale", &ParticleSystemNode::endSizeScale).range(0.0, 8.0)
        .tooltip("size multiplier at end of lifetime");
    t.property("stretch", &ParticleSystemNode::stretch).range(1.0, 32.0)
        .tooltip("vertical billboard stretch");
    t.property("drag", &ParticleSystemNode::drag).range(0.0, 32.0)
        .tooltip("linear velocity damping");
    t.property("noiseStrength", &ParticleSystemNode::noiseStrength).range(0.0, 100.0)
        .tooltip("procedural turbulence strength");
    t.property("noiseFrequency", &ParticleSystemNode::noiseFrequency).range(0.01, 100.0)
        .tooltip("procedural turbulence frequency");
    t.property("attractorPosition", &ParticleSystemNode::attractorPosition)
        .tooltip("world-space attractor position");
    t.property("attractorStrength", &ParticleSystemNode::attractorStrength).range(-100.0, 100.0)
        .tooltip("positive pulls toward attractor, negative pushes away");
    t.property("burstCount", &ParticleSystemNode::burstCount).range(0.0, 20000.0)
        .tooltip("particles spawned per burst; 0 uses an automatic budgeted count");
    t.property("blendMode", &ParticleSystemNode::blendMode)
        .enumValues({"Alpha", "Additive"})
        .tooltip("particle blending mode");
    t.property("looping", &ParticleSystemNode::looping);
    t.property("playing", &ParticleSystemNode::playing);
    t.signal("finished", &ParticleSystemNode::finished);
    t.slot("play", &ParticleSystemNode::play);
    t.slot("stop", &ParticleSystemNode::stop);
    t.slot("burst", &ParticleSystemNode::burst);
    t.slot("applyEffectPreset", &ParticleSystemNode::applyEffectPreset);
    t.slot("loadEffect", &ParticleSystemNode::loadEffect);
}

void ParticleSystemNode::play() {
    playing = true;
}

void ParticleSystemNode::stop() {
    playing = false;
}

void ParticleSystemNode::burst() {
    ++pendingBursts_;
    playing = true;
}

void ParticleSystemNode::applyEffectPreset() {
    ParticlePresetLibrary::apply(*this, effectClass);
    ++effectRevision_;
}

void ParticleSystemNode::loadEffect() {
    if (effectPath.empty()) return;
    try {
        ParticleEffect effect = ParticleEffect::loadFromFile(effectPath);
        if (!effect.applyTo(*this)) {
            Log::warn("ParticleSystemNode: effect has no emitter: ", effectPath);
        } else {
            ++effectRevision_;
        }
    } catch (const std::exception& e) {
        Log::warn("ParticleSystemNode: failed to load effect '", effectPath, "': ", e.what());
    }
}

uint32_t ParticleSystemNode::consumeBurstCount() {
    const uint32_t count = pendingBursts_;
    pendingBursts_ = 0;
    return count;
}

} // namespace saida
