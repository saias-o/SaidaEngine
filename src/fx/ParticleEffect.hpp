#pragma once

#include "nodes/ParticleSystemNode.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace saida {

enum class ParticleModuleType {
    SpawnRate,
    Burst,
    Shape,
    InitialVelocity,
    Gravity,
    Drag,
    Noise,
    ColorOverLife,
    SizeOverLife,
    Attractor,
    SubEmitter,
};

struct ParticleModule {
    ParticleModuleType type = ParticleModuleType::SpawnRate;
    bool enabled = true;
    nlohmann::json params = nlohmann::json::object();
};

struct ParticleEmitterDesc {
    std::string name = "Emitter";
    ParticleSystemNode::EffectClass effectClass = ParticleSystemNode::EffectClass::Simple;
    int maxParticles = 256;
    ParticleSystemNode::BlendMode blendMode = ParticleSystemNode::BlendMode::Additive;
    bool looping = true;
    std::vector<ParticleModule> modules;
};

class ParticleEffect {
public:
    static constexpr int kCurrentVersion = 1;

    std::string name = "Effect";
    int version = kCurrentVersion;
    std::vector<ParticleEmitterDesc> emitters;

    static ParticleEffect fromPreset(ParticleSystemNode::EffectClass effectClass);
    static ParticleEffect fromJson(const nlohmann::json& j);
    static ParticleEffect loadFromFile(const std::string& path);

    nlohmann::json toJson() const;
    bool saveToFile(const std::string& path) const;
    bool applyTo(ParticleSystemNode& node, size_t emitterIndex = 0) const;
    std::vector<std::string> validate() const;
};

const char* toString(ParticleModuleType type);
ParticleModuleType particleModuleTypeFromString(const std::string& text);
const char* toString(ParticleSystemNode::EffectClass effectClass);
ParticleSystemNode::EffectClass particleEffectClassFromString(const std::string& text);
const char* toString(ParticleSystemNode::BlendMode blendMode);
ParticleSystemNode::BlendMode particleBlendModeFromString(const std::string& text);

} // namespace saida
