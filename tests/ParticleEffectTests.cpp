#include "fx/ParticleEffect.hpp"
#include "fx/ParticleQuality.hpp"
#include "nodes/ParticleSystemNode.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {

bool require(bool ok) {
    return ok;
}

} // namespace

int main() {
    saida::ParticleEffect fire = saida::ParticleEffect::fromPreset(saida::ParticleSystemNode::EffectClass::Fire);
    if (!require(fire.name == "Fire")) return 1;
    if (!require(fire.emitters.size() == 1)) return 1;
    if (!require(fire.emitters[0].effectClass == saida::ParticleSystemNode::EffectClass::Fire)) return 1;
    if (!require(!fire.emitters[0].modules.empty())) return 1;
    if (!require(fire.validate().empty())) return 1;

    nlohmann::json saved = fire.toJson();
    if (!require(saved["format"].get<std::string>() == "SaidaFX")) return 1;
    if (!require(saved["emitters"][0]["modules"].is_array())) return 1;

    saida::ParticleEffect loaded = saida::ParticleEffect::fromJson(saved);
    if (!require(loaded.name == fire.name)) return 1;
    if (!require(loaded.emitters.size() == fire.emitters.size())) return 1;
    if (!require(loaded.emitters[0].modules.size() == fire.emitters[0].modules.size())) return 1;
    if (!require(loaded.emitters[0].blendMode == saida::ParticleSystemNode::BlendMode::Additive)) return 1;
    saida::ParticleSystemNode compiledFire;
    if (!require(loaded.applyTo(compiledFire))) return 1;
    if (!require(compiledFire.effectClass == saida::ParticleSystemNode::EffectClass::Fire)) return 1;
    if (!require(compiledFire.spawnRate == 120.0f)) return 1;
    if (!require(compiledFire.radius == 0.18f)) return 1;
    if (!require(compiledFire.emissive == 4.5f)) return 1;
    if (!require(compiledFire.shape == saida::ParticleSystemNode::Shape::Cone)) return 1;
    if (!require(compiledFire.drag > 0.0f)) return 1;

    saida::ParticleEffect explosion = saida::ParticleEffect::fromPreset(saida::ParticleSystemNode::EffectClass::Explosion);
    if (!require(!explosion.emitters[0].looping)) return 1;
    bool hasBurst = false;
    for (const saida::ParticleModule& m : explosion.emitters[0].modules) {
        hasBurst = hasBurst || m.type == saida::ParticleModuleType::Burst;
    }
    if (!require(hasBurst)) return 1;
    saida::ParticleSystemNode compiledExplosion;
    if (!require(explosion.applyTo(compiledExplosion))) return 1;
    if (!require(compiledExplosion.effectClass == saida::ParticleSystemNode::EffectClass::Explosion)) return 1;
    if (!require(!compiledExplosion.looping)) return 1;
    if (!require(compiledExplosion.spawnRate == 0.0f)) return 1;
    if (!require(compiledExplosion.burstCount == 320)) return 1;

    saida::ParticleEffect broken;
    broken.emitters.push_back({});
    broken.emitters[0].maxParticles = 0;
    broken.emitters[0].modules.push_back({saida::ParticleModuleType::SpawnRate, true, nlohmann::json::object()});
    if (!require(!broken.validate().empty())) return 1;

    saida::ParticleQualityBudget low = saida::particleQualityBudget(saida::QualityTier::Low);
    saida::ParticleQualityBudget ultra = saida::particleQualityBudget(saida::QualityTier::Ultra);
    if (!require(low.maxGpuParticles < ultra.maxGpuParticles)) return 1;
    if (!require(low.farUpdateInterval > ultra.farUpdateInterval)) return 1;

    saida::ParticleSystemNode node;
    node.maxParticles = 999999;
    if (!require(saida::particleEmitterCapacity(node, low) == low.maxParticlesPerEmitter)) return 1;

    const std::vector<std::string> templates = {
        "fire.saidafx", "magic.saidafx", "rain.saidafx", "snow.saidafx", "smoke.saidafx", "explosion.saidafx"
    };
    for (const std::string& name : templates) {
        saida::ParticleEffect fx = saida::ParticleEffect::loadFromFile(
            std::string(SAIDA_PROJECT_ROOT) + "/assets/fx/" + name);
        if (!require(fx.validate().empty())) return 1;
        saida::ParticleSystemNode templateNode;
        if (!require(fx.applyTo(templateNode))) return 1;
    }

    return 0;
}
