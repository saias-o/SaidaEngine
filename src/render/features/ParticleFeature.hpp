#pragma once

#include "fx/ParticleQuality.hpp"
#include "fx/ParticleRuntime.hpp"
#include "graphics/Pipeline.hpp"
#include "render/RenderFeature.hpp"
#include "nodes/ParticleSystemNode.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace saida {

class ParticleFeature : public ScenePassFeature {
public:
    ~ParticleFeature() override;
    void createPipelines(const RenderContext& ctx) override;
    void recordPrePass(const PrePassContext& pc) override;  // CPU sim + GPU compute
    void record(FrameContext& fc) override;                 // indirect draw only

private:
    struct EmitterState {
        float spawnAccumulator = 0.0f;
        float updateAccumulator = 0.0f;
        float lastTime = -1.0f;
        float timeSinceLastEmit = 1000000.0f;
        float simDtThisFrame = 0.0f;
        uint32_t spawnThisFrame = 0;
        uint64_t lastSeenSerial = 0;
        uint32_t effectRevision = 0;
        uint32_t seed = 1;
        bool emittedFinished = false;
        bool visibleThisFrame = true;
    };

    struct Push {
        uint32_t particleOffset = 0;
        uint32_t pad[3]{};
    };

    static constexpr uint32_t kFramesInFlightFallback = 2;
    static constexpr uint32_t kVertsPerParticle = 6;

    // Result of a blend batch's pre-pass compute, consumed by record()'s draw.
    struct BatchDraw {
        Pipeline* pipeline = nullptr;
        ParticleRuntime* runtime = nullptr;
        uint32_t parity = 0;
        bool draw = false;
    };

    rhi::Device* device_ = nullptr;
    ParticleQualityBudget budget_;
    uint64_t recordSerial_ = 0;
    std::unique_ptr<Pipeline> alphaPipeline_;
    std::unique_ptr<Pipeline> additivePipeline_;
    std::unique_ptr<ParticleRuntime> alphaRuntime_;
    std::unique_ptr<ParticleRuntime> additiveRuntime_;
    std::unordered_map<ParticleSystemNode*, EmitterState> states_;
    BatchDraw alphaBatch_;
    BatchDraw additiveBatch_;
};

} // namespace saida
