#pragma once

#include "nodes/ParticleSystemNode.hpp"

#include <glm/glm.hpp>

namespace saida {

struct ParticlePreset {
    ParticleSystemNode::EffectClass effectClass = ParticleSystemNode::EffectClass::Simple;
    int maxParticles = 256;
    float spawnRate = 48.0f;
    float lifetime = 1.5f;
    float startSpeed = 1.0f;
    float startSize = 0.2f;
    glm::vec4 startColor{1.0f};
    glm::vec4 endColor{1.0f, 1.0f, 1.0f, 0.0f};
    glm::vec3 gravity{0.0f};
    float radius = 0.25f;
    ParticleSystemNode::Shape shape = ParticleSystemNode::Shape::Sphere;
    glm::vec3 boxExtents{1.0f};
    float coneAngle = 30.0f;
    float ringThickness = 0.08f;
    float emissive = 1.0f;
    float endSizeScale = 0.35f;
    float stretch = 1.0f;
    float drag = 0.0f;
    float noiseStrength = 0.0f;
    float noiseFrequency = 1.0f;
    glm::vec3 attractorPosition{0.0f};
    float attractorStrength = 0.0f;
    int burstCount = 0;
    ParticleSystemNode::BlendMode blendMode = ParticleSystemNode::BlendMode::Additive;
    bool looping = true;
    bool playing = true;
};

class ParticlePresetLibrary {
public:
    static ParticlePreset presetFor(ParticleSystemNode::EffectClass effectClass);
    static void apply(ParticleSystemNode& node, ParticleSystemNode::EffectClass effectClass);
};

} // namespace saida
