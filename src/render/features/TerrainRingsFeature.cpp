#include "render/features/TerrainRingsFeature.hpp"

#include "core/Paths.hpp"
#include "core/Log.hpp"
#include "graphics/Material.hpp"
#include "graphics/ResourceManager.hpp"
#include "nodes/LightNode.hpp"
#include "render/Renderer.hpp"  // kMaxLights: a light's index in the lighting UBO
#include "scene/Scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace saida {

namespace {
// The light the rings shadow themselves from: the brightest directional one
// that casts shadows, among those the lighting UBO holds (its index there is
// its place in Scene::lights). Its direction toward the light, in world space.
// The GPU layouts carry flags as floats: present at 1, absent at 0.
bool flagged(float value) { return value > 0.5f; }

int shadowingLight(Scene& scene, glm::vec3& toLight) {
    int best = -1;
    float strongest = 0.0f;
    int index = 0;
    for (LightNode* light : scene.lights()) {
        if (index >= kMaxLights) break;
        const float power = light->intensity * std::max({light->color.r, light->color.g, light->color.b});
        if (light->type == LightType::Directional && light->castShadows && power > strongest) {
            strongest = power;
            best = index;
            toLight = -glm::normalize(glm::mat3(light->worldTransform()) * light->direction);
        }
        ++index;
    }
    return best;
}
}  // namespace

TerrainRingsFeature::~TerrainRingsFeature() = default;

bool TerrainRingsFeature::MarchedFor::holds(const MarchedFor& now) const {
    return node == now.node && revisions == now.revisions && light == now.light &&
           glm::dot(sun, now.sun) >= std::cos(kShadowTurn);
}

TerrainRingsFeature::MarchedFor TerrainRingsFeature::marchingNow(const FrameBuffers& f, uint32_t slot) const {
    const GpuTerrain& g = packed_[slot];
    const Uploaded& up = f.uploaded[slot];
    MarchedFor now{up.node, {}, glm::vec3(g.sun), g.sun.w};
    for (int k = 0; k < kLevels; ++k)
        now.revisions[size_t(k)] = flagged(g.levels[k].w) ? up.revisions[size_t(k)] : 0u;
    return now;
}

void TerrainRingsFeature::createPipelines(const RenderContext& ctx) {
    device_ = &ctx.device;
    resources_ = &ctx.resources;
    textured_ = resources_->globalMaterialSet() != VK_NULL_HANDLE;
    const uint32_t frames = std::max(1u, ctx.framesInFlight);

    // set 1: the terrains' UBO, their heights, their layers and their
    // sunlight, one copy per frame-in-flight so a frame never rewrites what
    // the GPU still reads. The sun pass reads the first two and writes the
    // last.
    setLayout_ = std::make_unique<rhi::BindGroupLayout>(*device_,
        std::vector<rhi::BindGroupLayoutEntry>{
            {0, rhi::BindingType::UniformBuffer,
             rhi::ShaderStages::Vertex | rhi::ShaderStages::Fragment | rhi::ShaderStages::Compute},
            {1, rhi::BindingType::StorageBuffer, rhi::ShaderStages::Vertex | rhi::ShaderStages::Compute},
            {2, rhi::BindingType::StorageBuffer, rhi::ShaderStages::Vertex | rhi::ShaderStages::Fragment},
            {3, rhi::BindingType::StorageBuffer, rhi::ShaderStages::Vertex | rhi::ShaderStages::Compute},
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
        // As many floats as heights; only the GPU writes and reads it.
        f.sunlight = std::make_unique<Buffer>(*device_, heightBytes, rhi::BufferUsage::Storage,
                                              MemoryUsage::GpuOnly);
        std::vector<rhi::BindGroupEntry> entries(4);
        entries[0].binding = 0; entries[0].buffer = f.ubo.get(); entries[0].range = uboBytes;
        entries[1].binding = 1; entries[1].buffer = f.heights.get(); entries[1].range = heightBytes;
        entries[2].binding = 2; entries[2].buffer = f.layers.get(); entries[2].range = layerBytes;
        entries[3].binding = 3; entries[3].buffer = f.sunlight.get(); entries[3].range = heightBytes;
        f.set = std::make_unique<rhi::BindGroup>(*setLayout_, entries);
    }

    Pipeline::Desc desc;
    desc.vertPath = shaderPath(ctx.stereo() ? "multiview.terrain_rings.vert.spv" : "terrain_rings.vert.spv");
    desc.fragPath = shaderPath(textured_ ? "bindless.terrain_rings.frag.spv" : "terrain_rings.frag.spv");
    desc.colorFormats = {ctx.colorFormat};
    desc.depthFormat = ctx.depthFormat;
    desc.bindGroupLayouts = {&ctx.globalSetLayout, setLayout_.get()};
    if (textured_) desc.bindGroupLayouts.push_back(resources_->globalMaterialSetLayout());
    desc.samples = ctx.samples;
    desc.vertexInput = false;
    // Seen from a summit or from under a ridge alike: no face is culled.
    desc.cullMode = rhi::CullMode::None;
    desc.pushConstantSize = sizeof(Push);
    desc.viewMask = ctx.viewMask;
    pipeline_ = std::make_unique<Pipeline>(ctx.device, desc);

    // The sun pass declares set 1 like the rings; set 0 is laid out, not bound.
    sunPipeline_ = std::make_unique<ComputePipeline>(*device_, shaderPath("terrain_rings_sun.comp.spv"),
        std::vector<rhi::vulkan::BindGroupLayoutRef>{ctx.globalSetLayout, *setLayout_}, uint32_t(sizeof(Push)));
    occlusionLayout_ = std::make_unique<rhi::BindGroupLayout>(*device_,
        std::vector<rhi::BindGroupLayoutEntry>{{0, rhi::BindingType::StorageImage, rhi::ShaderStages::Compute}});
    occlusionPipeline_ = std::make_unique<ComputePipeline>(*device_, shaderPath("terrain_rings_occlusion.comp.spv"),
        std::vector<rhi::vulkan::BindGroupLayoutRef>{ctx.globalSetLayout, *setLayout_, *occlusionLayout_},
        uint32_t(sizeof(Push)));
}

void TerrainRingsFeature::prepare(Scene& scene, uint32_t frameIndex) {
    std::array<TerrainRingsNode*, kMaxTerrains> nodes{};
    count_ = 0;
    for (TerrainRingsNode* t : scene.terrainRings()) {
        if (!t->isActiveInHierarchy() || !t->isVisibleInHierarchy()) continue;
        if (count_ >= kMaxTerrains) break;
        nodes[count_++] = t;
    }
    if (count_ == 0) return;

    glm::vec3 toLight(0.0f, 1.0f, 0.0f);
    const int light = shadowingLight(scene, toLight);

    FrameBuffers& f = frames_[std::min<size_t>(frameIndex, frames_.size() - 1)];
    packed_ = {};
    for (uint32_t slot = 0; slot < count_; ++slot) {
        TerrainRingsNode& node = *nodes[slot];
        GpuTerrain& g = packed_[slot];
        g.localToWorld = node.worldTransform();
        const int levels = std::clamp(node.levels, 1, kLevels);
        g.focus = glm::vec4(float(node.focus.x), float(node.focus.y), node.innerRadius, float(levels));
        for (int i = 0; i < kLayers; ++i) {
            const auto& layer = node.layers()[size_t(i)];
            if (layer.material && !textured_ && !materialFallbackReported_) {
                Log::warn("[TerrainRings] descriptor indexing unavailable; using layer albedo/roughness");
                materialFallbackReported_ = true;
            }
            g.layers[i] = glm::vec4(layer.albedo, layer.roughness);
            g.textures[i] = glm::vec4(layer.material ? float(layer.material->bindlessIndex() + 1u) : 0.0f,
                                      layer.textureScale, layer.waveAmplitude, layer.wavelength);
            g.macros[i] = glm::vec4(1.0f / layer.macroSize, layer.macroVariation, layer.macroNormalStrength, layer.water ? 1.0f : 0.0f);
            g.waterDynamics[i] = glm::vec4(float(layer.waveType),layer.waveIntensity,layer.windAngle,layer.gustStrength);
        }
        if (light >= 0) {
            const glm::vec3 local = glm::normalize(glm::inverse(glm::mat3(g.localToWorld)) * toLight);
            g.sun = glm::vec4(local, float(light + 1));
        }
        g.relief = glm::vec4(node.highest(), 0.0f, 0.0f, 0.0f);

        const auto& holes = node.holes();
        g.holeCount = glm::ivec4(int(holes.size()), 0, 0, 0);
        for (size_t h = 0; h < holes.size(); ++h) {
            const auto& q = holes[h];
            g.holes[2 * h] = glm::vec4(float(q[0].x), float(q[0].y), float(q[1].x), float(q[1].y));
            g.holes[2 * h + 1] = glm::vec4(float(q[2].x), float(q[2].y), float(q[3].x), float(q[3].y));
        }

        Uploaded& up = f.uploaded[slot];
        if (up.node != &node) up = Uploaded{&node, {}, {}};
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
    f.ubo->write(packed_.data(), sizeof(GpuTerrain) * count_);
}

void TerrainRingsFeature::recordPrePass(const PrePassContext& pc) {
    prepare(pc.scene, pc.frameIndex);
    prepared_ = true;
    if (count_ == 0) {
        if (pc.sunOcclusion) pc.sunOcclusion->light = -1;
        return;
    }

    FrameBuffers& f = frames_[std::min<size_t>(pc.frameIndex, frames_.size() - 1)];
    bool dispatched = false;
    for (uint32_t slot = 0; slot < count_; ++slot) {
        const GpuTerrain& g = packed_[slot];
        if (!flagged(g.sun.w)) continue;
        // Marched again when a level changed (a ring shadows the others) or
        // the light turned: never otherwise.
        const MarchedFor now = marchingNow(f, slot);
        Uploaded& up = f.uploaded[slot];
        if (up.sunlight.holds(now)) continue;

        rhi::ComputePassEncoder cp = pc.encoder.beginComputePass();
        cp.setPipeline(*sunPipeline_);
        cp.setBindGroup(1, *f.set);
        const uint32_t groups = ComputePipeline::groupCount(uint32_t(TerrainRingsNode::kSamples) *
                                                            TerrainRingsNode::kSamples, kSunGroupSize);
        for (int k = 0; k < kLevels; ++k) {
            if (!flagged(g.levels[k].w)) continue;
            Push push{slot, uint32_t(k)};
            cp.setPushConstants(&push, sizeof(Push));
            cp.dispatch(groups);
        }
        cp.end();
        up.sunlight = now;
        dispatched = true;
    }
    if (pc.sunOcclusion) {
        if (recordOcclusion(pc, f)) dispatched = true;
    }
    if (dispatched) pc.encoder.computeToGraphicsBarrier();
}

bool TerrainRingsFeature::recordOcclusion(const PrePassContext& pc, FrameBuffers& f) {
    SunOcclusionMap& map = *pc.sunOcclusion;
    const GpuTerrain& g = packed_[0];
    // Only the first terrain, with its finest level, under a light it shadows.
    if (count_ == 0 || !flagged(g.sun.w) || !flagged(g.levels[0].w) || !map.view) {
        map.light = -1;
        return false;
    }
    const glm::vec4 L0 = g.levels[0];
    const float extent = float(TerrainRingsNode::kResolution) * L0.z;
    glm::mat4 toMap(0.0f);  // node-local (x, y, z) -> (u, v, height)
    toMap[0][0] = 1.0f / extent;
    toMap[2][1] = 1.0f / extent;
    toMap[1][2] = 1.0f;
    toMap[3] = glm::vec4(-L0.x / extent, -L0.y / extent, 0.0f, 1.0f);
    map.worldToMap = toMap * glm::inverse(g.localToWorld);
    map.light = int(std::lround(g.sun.w)) - 1;

    const MarchedFor now = marchingNow(f, 0);
    if (occlusion_.holds(now)) return false;

    if (!occlusionSet_ || occlusionView_ != map.view) {
        rhi::BindGroupEntry image;
        image.binding = 0;
        image.view = map.view;
        image.textureState = rhi::ResourceState::StorageReadWrite;
        occlusionSet_ = std::make_unique<rhi::BindGroup>(*occlusionLayout_, std::vector<rhi::BindGroupEntry>{image});
        occlusionView_ = map.view;
    }
    // Earlier frames still in flight sample the map: they finish first.
    pc.encoder.graphicsToComputeBarrier();
    rhi::ComputePassEncoder cp = pc.encoder.beginComputePass();
    cp.setPipeline(*occlusionPipeline_);
    cp.setBindGroup(1, *f.set);
    cp.setBindGroup(2, *occlusionSet_);
    Push push{0u, 0u};
    cp.setPushConstants(&push, sizeof(Push));
    const uint32_t groups = ComputePipeline::groupCount(SunOcclusionMap::kSize, kOcclusionGroupSize);
    cp.dispatch(groups, groups);
    cp.end();
    occlusion_ = now;
    return true;
}

void TerrainRingsFeature::record(FrameContext& fc) {
    // A frame recorded without the pre-pass draws its rings unshadowed rather
    // than over sunlight marched for other heights.
    if (!prepared_) {
        prepare(fc.scene, fc.frameIndex);
        for (uint32_t slot = 0; slot < count_; ++slot) packed_[slot].sun.w = 0.0f;
        FrameBuffers& f = frames_[std::min<size_t>(fc.frameIndex, frames_.size() - 1)];
        if (count_ > 0) f.ubo->write(packed_.data(), sizeof(GpuTerrain) * count_);
    }
    prepared_ = false;
    if (count_ == 0) return;

    FrameBuffers& f = frames_[std::min<size_t>(fc.frameIndex, frames_.size() - 1)];
    fc.pass.setPipeline(*pipeline_);
    fc.pass.setBindGroup(0, *fc.globalSet);
    fc.pass.setBindGroup(1, *f.set);
    if (textured_) fc.pass.setBindGroup(2, resources_->globalMaterialSet());
    const uint32_t vertices = uint32_t(TerrainRingsNode::kResolution) * TerrainRingsNode::kResolution * kVerticesPerCell;
    for (uint32_t slot = 0; slot < count_; ++slot)
        for (int k = 0; k < kLevels; ++k) {
            if (!flagged(packed_[slot].levels[k].w)) continue;
            Push pc{slot, uint32_t(k), fc.time};
            fc.pass.setPushConstants(&pc, sizeof(Push));
            fc.pass.draw(vertices);
        }
}

} // namespace saida
