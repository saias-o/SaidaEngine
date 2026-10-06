#pragma once

#include "render/RenderFeature.hpp"
#include "graphics/Buffer.hpp"    // Buffer is a class (Vulkan) or alias (web) — not fwd-declarable
#include "graphics/Pipeline.hpp"
#include "nodes/GrassNode.hpp"
#include "rhi/Rhi.hpp"

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <vector>

namespace saida {

// Render feature for GrassNode: the blades of every field near the camera, in
// three rings of procedural cells (grass.vert). A field's ground and cover
// live in storage buffers, rewritten only when its revision changes; a ring
// is drawn only over the cells where the field and the camera's reach meet.
class GrassFeature : public ScenePassFeature {
public:
    static constexpr uint32_t kMaxFields = 9;    // MAX_FIELDS in grass.glsl
    static constexpr int kRingCells = 192;       // RING_CELLS in grass.glsl; divisible by 4
    static constexpr int kRings = 3;
    // A blade's vertices in each ring: two segments and a tip near, the tip alone past it.
    static constexpr std::array<uint32_t, kRings> kBladeVertices{9u, 3u, 3u};

    ~GrassFeature() override;
    void createPipelines(const RenderContext& ctx) override;
    void record(FrameContext& fc) override;

    // The cells of ring `ring` a field covering node-local box [lo, hi] needs,
    // around a camera at node-local `eye` (x, z): what is drawn of it. Empty
    // (count 0) when the field is out of reach. Exposed for the tests.
    struct RingDraw {
        glm::vec2 origin{0.0f};   // node-local x, z of cell (0, 0)'s corner
        float spacing = 0.0f;
        glm::ivec2 cellMin{0};
        glm::ivec2 cells{0};      // drawn, along x and z
        glm::vec2 hole{0.0f};     // half-extent of the ring inside, 0 for the first
    };
    static RingDraw ringDraw(int ring, float density, float radius, glm::vec2 eye, glm::vec2 lo, glm::vec2 hi);

private:
    // Mirrors GpuGrass in grass.glsl; all-vec4 keeps std140 padding explicit.
    struct GpuGrass {
        glm::mat4 localToWorld{1.0f};
        glm::vec4 uvFromLocalU{0.0f};
        glm::vec4 uvFromLocalV{0.0f};
        glm::vec4 blades{0.0f};
        glm::vec4 wind{0.0f};
        glm::vec4 camera{0.0f};
        glm::ivec4 sizes{0};
        glm::vec4 tuft{0.0f};
        glm::vec4 benders[GrassNode::kMaxBenders]{};
    };
    struct Push {
        glm::vec2 origin;
        float spacing;
        uint32_t slot;
        glm::ivec2 cellMin;
        uint32_t cellsX;
        uint32_t ring;
        glm::vec2 hole;
        uint32_t vertices;
        uint32_t pad;
    };
    static_assert(sizeof(Push) == 48, "Push must match grass.glsl");

    struct FrameBuffers {
        std::unique_ptr<Buffer> ubo;
        std::unique_ptr<Buffer> ground;
        std::unique_ptr<Buffer> cover;
        std::unique_ptr<rhi::BindGroup> set;
        // What each slot holds: a field once per buffer after it changes.
        std::array<const GrassNode*, kMaxFields> nodes{};
        std::array<uint64_t, kMaxFields> revisions{};
    };
    struct Drawn {
        uint32_t slot;
        uint32_t tuftBlades;
        float density, radius;
        glm::vec2 eye, lo, hi;
    };

    rhi::Device* device_ = nullptr;
    std::unique_ptr<Pipeline> pipeline_;
    std::unique_ptr<rhi::BindGroupLayout> setLayout_;
    std::vector<FrameBuffers> frames_;
    std::array<GpuGrass, kMaxFields> packed_{};
    std::vector<Drawn> drawn_;
    bool overflowReported_ = false;
};

} // namespace saida
