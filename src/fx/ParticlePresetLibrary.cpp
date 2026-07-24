#include "fx/ParticlePresetLibrary.hpp"

namespace saida {

ParticlePreset ParticlePresetLibrary::presetFor(ParticleSystemNode::EffectClass effectClass) {
    using Effect = ParticleSystemNode::EffectClass;
    using Blend = ParticleSystemNode::BlendMode;
    using Shape = ParticleSystemNode::Shape;

    ParticlePreset p;
    p.effectClass = effectClass;

    switch (effectClass) {
        case Effect::Fire:
            p.maxParticles = 512;
            p.spawnRate = 120.0f;
            p.lifetime = 1.05f;
            p.startSpeed = 1.05f;
            p.startSize = 0.22f;
            p.startColor = {1.0f, 0.48f, 0.08f, 1.0f};
            p.endColor = {0.18f, 0.02f, 0.0f, 0.0f};
            p.gravity = {0.0f, 1.25f, 0.0f};
            p.radius = 0.18f;
            p.shape = Shape::Cone;
            p.coneAngle = 18.0f;
            p.emissive = 4.5f;
            p.endSizeScale = 0.12f;
            p.stretch = 1.15f;
            p.drag = 0.28f;
            p.noiseStrength = 0.08f;
            p.noiseFrequency = 2.0f;
            p.blendMode = Blend::Additive;
            break;
        case Effect::Magic:
            p.maxParticles = 768;
            p.spawnRate = 84.0f;
            p.lifetime = 1.85f;
            p.startSpeed = 0.9f;
            p.startSize = 0.16f;
            p.startColor = {0.2f, 0.85f, 1.0f, 1.0f};
            p.endColor = {0.85f, 0.25f, 1.0f, 0.0f};
            p.gravity = {0.0f, 0.05f, 0.0f};
            p.radius = 0.55f;
            p.shape = Shape::Ring;
            p.ringThickness = 0.16f;
            p.emissive = 5.0f;
            p.endSizeScale = 0.4f;
            p.stretch = 1.25f;
            p.drag = 0.08f;
            p.noiseStrength = 0.55f;
            p.noiseFrequency = 2.6f;
            p.attractorStrength = 0.15f;
            p.blendMode = Blend::Additive;
            break;
        case Effect::Rain:
            p.maxParticles = 1400;
            p.spawnRate = 760.0f;
            p.lifetime = 1.35f;
            p.startSpeed = 8.5f;
            p.startSize = 0.055f;
            p.startColor = {0.55f, 0.78f, 1.0f, 0.78f};
            p.endColor = {0.55f, 0.78f, 1.0f, 0.18f};
            p.gravity = {0.0f, -14.0f, 0.0f};
            p.radius = 8.0f;
            p.shape = Shape::Disc;
            p.emissive = 1.25f;
            p.endSizeScale = 1.0f;
            p.stretch = 9.0f;
            p.blendMode = Blend::Alpha;
            break;
        case Effect::Snow:
            p.maxParticles = 1100;
            p.spawnRate = 180.0f;
            p.lifetime = 5.0f;
            p.startSpeed = 1.15f;
            p.startSize = 0.08f;
            p.startColor = {0.96f, 0.98f, 1.0f, 0.85f};
            p.endColor = {0.96f, 0.98f, 1.0f, 0.2f};
            p.gravity = {0.0f, -0.85f, 0.0f};
            p.radius = 8.0f;
            p.shape = Shape::Disc;
            p.emissive = 1.0f;
            p.endSizeScale = 0.65f;
            p.stretch = 1.0f;
            p.drag = 0.12f;
            p.noiseStrength = 0.18f;
            p.noiseFrequency = 0.7f;
            p.blendMode = Blend::Alpha;
            break;
        case Effect::Smoke:
            p.maxParticles = 512;
            p.spawnRate = 54.0f;
            p.lifetime = 3.6f;
            p.startSpeed = 0.48f;
            p.startSize = 0.46f;
            p.startColor = {0.42f, 0.42f, 0.42f, 0.36f};
            p.endColor = {0.08f, 0.08f, 0.08f, 0.0f};
            p.gravity = {0.0f, 0.28f, 0.0f};
            p.radius = 0.34f;
            p.shape = Shape::Sphere;
            p.emissive = 0.7f;
            p.endSizeScale = 2.25f;
            p.stretch = 1.35f;
            p.drag = 0.35f;
            p.noiseStrength = 0.22f;
            p.noiseFrequency = 1.4f;
            p.blendMode = Blend::Alpha;
            break;
        case Effect::Explosion:
            p.maxParticles = 900;
            p.spawnRate = 0.0f;
            p.lifetime = 1.15f;
            p.startSpeed = 6.0f;
            p.startSize = 0.34f;
            p.startColor = {1.0f, 0.62f, 0.12f, 1.0f};
            p.endColor = {0.09f, 0.015f, 0.0f, 0.0f};
            p.gravity = {0.0f, -2.4f, 0.0f};
            p.radius = 0.18f;
            p.shape = Shape::Sphere;
            p.emissive = 6.0f;
            p.endSizeScale = 0.08f;
            p.stretch = 1.6f;
            p.drag = 0.55f;
            p.noiseStrength = 0.18f;
            p.noiseFrequency = 3.0f;
            p.burstCount = 320;
            p.blendMode = Blend::Additive;
            p.looping = false;
            p.playing = false;
            break;
        case Effect::Simple:
        default:
            p.maxParticles = 256;
            p.spawnRate = 48.0f;
            p.lifetime = 1.5f;
            p.startSpeed = 1.0f;
            p.startSize = 0.2f;
            p.startColor = {1.0f, 0.85f, 0.35f, 1.0f};
            p.endColor = {1.0f, 0.15f, 0.02f, 0.0f};
            p.gravity = {0.0f, -0.3f, 0.0f};
            p.radius = 0.25f;
            p.shape = Shape::Sphere;
            p.emissive = 2.0f;
            p.endSizeScale = 0.35f;
            p.stretch = 1.0f;
            p.blendMode = Blend::Additive;
            break;
    }

    return p;
}

void ParticlePresetLibrary::apply(ParticleSystemNode& node, ParticleSystemNode::EffectClass effectClass) {
    const ParticlePreset p = presetFor(effectClass);
    node.effectClass = p.effectClass;
    node.maxParticles = p.maxParticles;
    node.spawnRate = p.spawnRate;
    node.lifetime = p.lifetime;
    node.startSpeed = p.startSpeed;
    node.startSize = p.startSize;
    node.startColor = p.startColor;
    node.endColor = p.endColor;
    node.gravity = p.gravity;
    node.radius = p.radius;
    node.shape = p.shape;
    node.boxExtents = p.boxExtents;
    node.coneAngle = p.coneAngle;
    node.ringThickness = p.ringThickness;
    node.emissive = p.emissive;
    node.endSizeScale = p.endSizeScale;
    node.stretch = p.stretch;
    node.drag = p.drag;
    node.noiseStrength = p.noiseStrength;
    node.noiseFrequency = p.noiseFrequency;
    node.attractorPosition = p.attractorPosition;
    node.attractorStrength = p.attractorStrength;
    node.burstCount = p.burstCount;
    node.blendMode = p.blendMode;
    node.looping = p.looping;
    node.playing = p.playing;
}

} // namespace saida
