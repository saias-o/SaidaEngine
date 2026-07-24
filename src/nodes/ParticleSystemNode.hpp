#pragma once

#include "core/Reflection.hpp"
#include "core/Signal.hpp"
#include "scene/Node.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace saida {

class ParticleSystemNode : public Node {
public:
    ParticleSystemNode() : Node("ParticleSystem") {}

    SAIDA_REFLECT_NODE(ParticleSystemNode, "ParticleSystem")

    enum class EffectClass {
        Simple = 0,
        Fire,
        Magic,
        Rain,
        Snow,
        Smoke,
        Explosion,
    };

    enum class BlendMode {
        Alpha = 0,
        Additive,
    };

    enum class Shape {
        Point = 0,
        Sphere,
        Disc,
        Box,
        Cone,
        Ring,
    };

    EffectClass effectClass = EffectClass::Simple;
    std::string effectPath;
    int maxParticles = 256;
    float spawnRate = 48.0f;
    float lifetime = 1.5f;
    float startSpeed = 1.0f;
    float startSize = 0.2f;
    glm::vec4 startColor{1.0f, 0.85f, 0.35f, 1.0f};
    glm::vec4 endColor{1.0f, 0.15f, 0.02f, 0.0f};
    glm::vec3 gravity{0.0f, -0.3f, 0.0f};
    float radius = 0.25f;
    Shape shape = Shape::Sphere;
    glm::vec3 boxExtents{1.0f};
    float coneAngle = 30.0f;
    float ringThickness = 0.08f;
    float emissive = 2.0f;
    float endSizeScale = 0.35f;
    float stretch = 1.0f;
    float drag = 0.0f;
    float noiseStrength = 0.0f;
    float noiseFrequency = 1.0f;
    glm::vec3 attractorPosition{0.0f};
    float attractorStrength = 0.0f;
    int burstCount = 0;
    BlendMode blendMode = BlendMode::Additive;
    bool looping = true;
    bool playing = true;

    Signal<> finished;

    void play();
    void stop();
    void burst();
    void applyEffectPreset();
    void loadEffect();

    uint32_t consumeBurstCount();
    uint32_t effectRevision() const { return effectRevision_; }

private:
    uint32_t pendingBursts_ = 0;
    uint32_t effectRevision_ = 1;
};

} // namespace saida
