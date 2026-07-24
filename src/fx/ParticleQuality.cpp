#include "fx/ParticleQuality.hpp"

#include "nodes/ParticleSystemNode.hpp"

#include <algorithm>

namespace saida {

ParticleQualityBudget particleQualityBudget(QualityTier tier) {
    switch (tier) {
        case QualityTier::Ultra:
            return {65536, 256, 4096, 48.0f, 0.0f};
        case QualityTier::High:
            return {32768, 192, 2048, 32.0f, 1.0f / 30.0f};
        case QualityTier::Medium:
            return {16384, 128, 1024, 22.0f, 1.0f / 20.0f};
        case QualityTier::Low:
        default:
            return {8192, 64, 512, 14.0f, 1.0f / 15.0f};
    }
}

uint32_t particleEmitterCapacity(const ParticleSystemNode& node, const ParticleQualityBudget& budget) {
    const uint32_t requested = static_cast<uint32_t>(std::max(0, node.maxParticles));
    return std::min(requested, budget.maxParticlesPerEmitter);
}

} // namespace saida
