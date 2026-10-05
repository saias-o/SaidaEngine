#include "render/features/TerrainRingsFeature.hpp"

#include "core/Paths.hpp"
#include "scene/Scene.hpp"

#include <algorithm>
#include <cstring>

namespace saida {

TerrainRingsFeature::~TerrainRingsFeature() = default;

void TerrainRingsFeature::createPipelines(const RenderContext& ctx) {
    device_ = &ctx.device;
    const uint32_t frames = std::max(1u, ctx.framesInFlight);

    // set 1: the terrains' UBO (both stages), their heights and their layers
    // (vertex stage), one copy per frame-in-flight so a frame never rewrites
    // what the GPU still reads.
    setLayout_ = std::make_unique<rhi::BindGroupLayout>(*device_,
        std::vector<rhi::BindGroupLayoutEntry>{
            {0, rhi::BindingType::UniformBuffer, rhi::ShaderStages::Vertex | rhi::ShaderStages::Fragment},
            {1, rhi::BindingType::StorageBuffer, rhi::ShaderStages::Vertex},
            {2, rhi::BindingType::StorageBuffer, rhi::ShaderStages::Vertex},
        });

    const uint64_t uboBytes = sizeof(GpuTerrain) * kMaxTerrains;
    const uint64_t heightBytes = kLevelHeightBytes * kLevels * kMaxTerrains;
    const uint64_t layerBytes = kLevelLayerBytes * kLevels * kMaxTerrains;
    frames_.resize(frames);
    for (FrameBuffers& f : frames_) {
        f.ubo = std::make_unique<Buffer>(*device_, uboBytes, rhi::BufferUsage::Uniform, MemoryUsage::HostVisible);
        f.heights = std::make_unique<Buffer>(*device_, heightBytes, rhi::BufferUsage::Storage,
                                             MemoryUsage::HostVisible);
        f.layers = std::make_unique<Buffer>(*device_, layerBytes, rhi::BufferUsage::Storage,
                                            MemoryUsage::HostVisible);
        std::vector<rhi::BindGroupEntry> entries(3);
        entries[0].binding = 0; entries[0].buffer = f.ubo.get(); entries[0].range = uboBytes;
        entries[1].binding = 1; entries[1].buffer = f.heights.get(); entries[1].range = heightBytes;
        entries[2].binding = 2; entries[2].buffer = f.layers.get(); entries[2].range = layerBytes;
        f.set = std::make_unique<rhi::BindGroup>(*setLayout_, entries);
    }

    Pipeline::Desc desc;
    desc.vertPath = shaderPath(ctx.stereo() ? "multiview.terrain_rings.vert.spv" : "terrain_rings.vert.spv");
    desc.fragPath = shaderPath("terrain_rings.frag.spv");
    desc.colorFormats = {ctx.colorFormat};
    desc.depthFormat = ctx.depthFormat;
    desc.bindGroupLayouts = {&ctx.globalSetLayout, setLayout_.get()};
    desc.samples = ctx.samples;
    desc.vertexInput = false;
    // Seen from a summit or from under a ridge alike: no face is culled.
    desc.cullMode = rhi::CullMode::None;
    desc.pushConstantSize = sizeof(Push);
    desc.viewMask = ctx.viewMask;
    pipeline_ = std::make_unique<Pipeline>(ctx.device, desc);
}

void TerrainRingsFeature::record(FrameContext& fc) {
    std::array<TerrainRingsNode*, kMaxTerrains> nodes{};
    uint32_t count = 0;
    for (TerrainRingsNode* t : fc.scene.terrainRings()) {
        if (!t->isActiveInHierarchy() || !t->isVisibleInHierarchy()) continue;
        if (count >= kMaxTerrains) break;
        nodes[count++] = t;
    }
    if (count == 0) return;

    FrameBuffers& f = frames_[std::min<size_t>(fc.frameIndex, frames_.size() - 1)];
    std::array<GpuTerrain, kMaxTerrains> packed{};
    for (uint32_t slot = 0; slot < count; ++slot) {
        TerrainRingsNode& node = *nodes[slot];
        GpuTerrain& g = packed[slot];
        g.localToWorld = node.worldTransform();
        const int levels = std::clamp(node.levels, 1, kLevels);
        g.focus = glm::vec4(float(node.focus.x), float(node.focus.y), node.innerRadius, float(levels));
        for (int i = 0; i < kLayers; ++i)
            g.layers[i] = glm::vec4(node.layers()[size_t(i)].albedo, node.layers()[size_t(i)].roughness);

        const auto& holes = node.holes();
        g.holeCount = glm::ivec4(int(holes.size()), 0, 0, 0);
        for (size_t h = 0; h < holes.size(); ++h) {
            const auto& q = holes[h];
            g.holes[2 * h] = glm::vec4(float(q[0].x), float(q[0].y), float(q[1].x), float(q[1].y));
            g.holes[2 * h + 1] = glm::vec4(float(q[2].x), float(q[2].y), float(q[3].x), float(q[3].y));
        }

        Uploaded& up = f.uploaded[slot];
        if (up.node != &node) up = Uploaded{&node, {}};
        for (int k = 0; k < levels; ++k) {
            const TerrainRingsNode::Level* level = node.level(k);
            if (!level) continue;
            g.levels[k] = glm::vec4(float(level->origin.x), float(level->origin.y), float(level->spacing), 1.0f);
            if (up.revisions[size_t(k)] == node.revision(k)) continue;
            const uint64_t at = uint64_t(slot) * kLevels + uint64_t(k);
            f.heights->write(level->heights.data(), kLevelHeightBytes, at * kLevelHeightBytes);
            packedLayers_.assign(size_t(kLevelLayerBytes / 4), 0u);
            for (size_t c = 0; c < level->layers.size(); ++c)
                packedLayers_[c >> 2] |= uint32_t(level->layers[c]) << ((c & 3u) * 8u);
            f.layers->write(packedLayers_.data(), kLevelLayerBytes, at * kLevelLayerBytes);
            up.revisions[size_t(k)] = node.revision(k);
        }
    }
    f.ubo->write(packed.data(), sizeof(GpuTerrain) * count);

    fc.pass.setPipeline(*pipeline_);
    fc.pass.setBindGroup(0, *fc.globalSet);
    fc.pass.setBindGroup(1, *f.set);
    const uint32_t vertices = uint32_t(TerrainRingsNode::kResolution) * TerrainRingsNode::kResolution * 6u;
    for (uint32_t slot = 0; slot < count; ++slot)
        for (int k = 0; k < kLevels; ++k) {
            if (packed[slot].levels[k].w < 0.5f) continue;
            Push pc{slot, uint32_t(k)};
            fc.pass.setPushConstants(&pc, sizeof(Push));
            fc.pass.draw(vertices);
        }
}

} // namespace saida
