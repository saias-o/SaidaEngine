#pragma once

#include "rhi/Capabilities.hpp"

#include <cstdint>

namespace saida {

class ParticleSystemNode;

struct ParticleQualityBudget {
    uint32_t maxGpuParticles = 8192;
    uint32_t maxEmitters = 64;
    uint32_t maxParticlesPerEmitter = 512;
    float farUpdateDistance = 12.0f;
    float farUpdateInterval = 1.0f / 15.0f;
};

ParticleQualityBudget particleQualityBudget(QualityTier tier);
uint32_t particleEmitterCapacity(const ParticleSystemNode& node, const ParticleQualityBudget& budget);

} // namespace saida
