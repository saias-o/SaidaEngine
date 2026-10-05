#pragma once

#include "render/RenderFeature.hpp"
#include "graphics/Buffer.hpp"    // Buffer is a class (Vulkan) or alias (web) — not fwd-declarable
#include "graphics/Pipeline.hpp"
#include "nodes/TerrainRingsNode.hpp"
#include "rhi/Rhi.hpp"

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <vector>

namespace saida {

// Render feature for TerrainRingsNode: each present level is one draw of a
// procedural grid (no vertex input), its heights and layers read from storage
// buffers that are rewritten only where a level's revision changed.
//
// The rings' own shadow: before the scene pass, terrain_rings_sun.comp
// marches from every sample toward the brightest directional light that casts
// shadows, and only when a level changed or that light turned by more than
// kShadowTurn since -- a Sun turns that far in about twenty seconds.
class TerrainRingsFeature : public ScenePassFeature {
public:
    static constexpr uint32_t kMaxTerrains = 2;  // MAX_TERRAINS in terrain_rings.glsl
    // Radians the light may turn before the shadow is marched again.
    static constexpr float kShadowTurn = 0.0017f;  // 0.1 degree

    ~TerrainRingsFeature() override;
    void createPipelines(const RenderContext& ctx) override;
    void recordPrePass(const PrePassContext& pc) override;
    void record(FrameContext& fc) override;

private:
    static constexpr int kLevels = TerrainRingsNode::kMaxLevels;
    static constexpr int kLayers = TerrainRingsNode::kMaxLayers;
    static constexpr uint64_t kLevelHeightBytes =
        uint64_t(TerrainRingsNode::kSamples) * TerrainRingsNode::kSamples * sizeof(float);
    static constexpr uint64_t kLevelLayerBytes =
        uint64_t(TerrainRingsNode::kResolution) * TerrainRingsNode::kResolution;  // one byte a cell

    // Mirrors GpuTerrain in terrain_rings.glsl; all-vec4 keeps std140 padding explicit.
    struct GpuTerrain {
        glm::mat4 localToWorld{1.0f};
        glm::vec4 focus{0.0f};
        glm::vec4 levels[kLevels]{};
        glm::vec4 layers[kLayers]{};
        glm::vec4 holes[TerrainRingsNode::kMaxHoles * 2]{};
        glm::ivec4 holeCount{0};
        glm::vec4 sun{0.0f};     // xyz toward the shadowing light, node-local; w its index + 1, 0 none
        glm::vec4 relief{0.0f};  // x the highest sample
    };
    struct Push {
        uint32_t slot;
        uint32_t level;
    };

    // What one frame-in-flight's buffers hold, so a level is written once
    // per buffer after it changes, and never otherwise.
    // `shadowed` and `shadowSun` say what its sunlight was marched for.
    struct Uploaded {
        const TerrainRingsNode* node = nullptr;
        std::array<uint64_t, kLevels> revisions{};
        std::array<uint64_t, kLevels> shadowed{};
        glm::vec3 shadowSun{0.0f};
        float shadowLight = 0.0f;
    };
    struct FrameBuffers {
        std::unique_ptr<Buffer> ubo;
        std::unique_ptr<Buffer> heights;
        std::unique_ptr<Buffer> layers;
        std::unique_ptr<Buffer> sunlight;
        std::unique_ptr<rhi::BindGroup> set;
        std::array<Uploaded, kMaxTerrains> uploaded{};
    };

    // Packs and uploads this frame's terrains into `packed_` / `count_`.
    void prepare(Scene& scene, uint32_t frameIndex);

    rhi::Device* device_ = nullptr;
    std::unique_ptr<Pipeline> pipeline_;
    std::unique_ptr<ComputePipeline> sunPipeline_;
    std::array<GpuTerrain, kMaxTerrains> packed_{};
    uint32_t count_ = 0;
    bool prepared_ = false;  // by recordPrePass, for this frame's record
    std::unique_ptr<rhi::BindGroupLayout> setLayout_;
    std::vector<FrameBuffers> frames_;
    std::vector<uint32_t> packedLayers_;  // scratch: one level's layers, four to a word
};

} // namespace saida
