#include "render/features/WaterFeature.hpp"

#include "core/Paths.hpp"
#include "core/Log.hpp"
#include "graphics/Buffer.hpp"
#include "graphics/Mesh.hpp"
#ifndef SAIDA_RHI_WEBGPU
#include "graphics/VulkanDevice.hpp"
#include "rhi/vulkan/Format.hpp"
#endif
#include "scene/Scene.hpp"
#include "scene/Node.hpp"
#include "nodes/WaterNode.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <vector>

namespace saida {

namespace {
uint64_t surfaceSignature(const std::string& values) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char byte : values) hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}

bool parseSurface(const std::string& encoded, std::vector<Vertex>& vertices) {
    std::vector<float> values;
    values.reserve(encoded.size() / 8);
    const char* cursor = encoded.c_str();
    while (*cursor) {
        while (std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
        if (!*cursor) break;
        char* end = nullptr;
        const float value = std::strtof(cursor, &end);
        if (end == cursor || !std::isfinite(value)) return false;
        values.push_back(value);
        cursor = end;
    }
    if (values.empty() || values.size() % 9 != 0 || values.size() / 3 > std::numeric_limits<uint32_t>::max())
        return false;
    vertices.resize(values.size() / 3);
    for (size_t i = 0; i < vertices.size(); ++i)
        vertices[i].pos = {values[i * 3], values[i * 3 + 1], values[i * 3 + 2]};
    return true;
}
} // namespace

WaterFeature::~WaterFeature() = default;

void WaterFeature::createPipelines(const RenderContext& ctx) {
    device_ = &ctx.device;
    const uint32_t frames = std::max(1u, ctx.framesInFlight);
    framesInFlight_ = frames;

    // set 1: the per-node water UBO array (vertex needs it for waves + shore flatten,
    // fragment for shading), one buffer + set per frame-in-flight so a frame never
    // rewrites data the GPU is still reading.
#ifdef SAIDA_RHI_WEBGPU
    setLayout_ = std::make_unique<rhi::BindGroupLayout>(*device_,
        std::vector<rhi::webgpu::BindGroupLayoutEntry>{
            {0, rhi::BindingType::UniformBuffer, rhi::ShaderStages::Vertex | rhi::ShaderStages::Fragment},
        });
#else
    setLayout_ = std::make_unique<rhi::BindGroupLayout>(*device_,
        std::vector<rhi::BindGroupLayoutEntry>{
            {0, rhi::BindingType::UniformBuffer, rhi::ShaderStages::Vertex | rhi::ShaderStages::Fragment},
        });
#endif

    ubos_.resize(frames);
    sets_.resize(frames);
    const uint64_t bufSize = sizeof(GpuWater) * kMaxWaters;
    for (uint32_t i = 0; i < frames; ++i) {
        ubos_[i] = std::make_unique<Buffer>(*device_, bufSize,
            rhi::BufferUsage::Uniform, MemoryUsage::HostVisible);

        rhi::BindGroupEntry entry;
        entry.binding = 0;
        entry.buffer = ubos_[i].get();
        entry.range = bufSize;
        sets_[i] = std::make_unique<rhi::BindGroup>(*setLayout_, std::vector<rhi::BindGroupEntry>{entry});
    }

    // set 0 = global (camera + lighting + env); set 1 = the water UBO array. Look/feel
    // is data; the per-draw push is just the node index + time. Procedural grid (no
    // vertex input), depth-tested + depth-writing, two-sided, and ALPHA-BLENDED so the
    // shore can dissolve into the wet sand.
    const char* vert = ctx.stereo() ? "multiview.water.vert.spv" : "water.vert.spv";
    Pipeline::Desc desc;
    desc.vertPath = shaderPath(vert);
    desc.fragPath = shaderPath("water.frag.spv");
    desc.colorFormats = {ctx.colorFormat};
    desc.depthFormat = ctx.depthFormat;
    desc.bindGroupLayouts = {&ctx.globalSetLayout, setLayout_.get()};
    desc.samples = ctx.samples;
    desc.vertexInput = false;
    desc.cullMode = rhi::CullMode::None;
    desc.blendMode = rhi::BlendMode::Alpha;
    desc.pushConstantSize = sizeof(Push);
    desc.viewMask = ctx.viewMask;
    realisticPipeline_ = std::make_unique<Pipeline>(ctx.device, desc);

    desc.vertPath = shaderPath(ctx.stereo()
        ? "multiview.cartoon_water.vert.spv"
        : "cartoon_water.vert.spv");
    desc.fragPath = shaderPath("cartoon_water.frag.spv");
    cartoonPipeline_ = std::make_unique<Pipeline>(ctx.device, desc);

    desc.vertexInput = true;
    desc.vertPath = shaderPath(ctx.stereo()
        ? "multiview.water_surface.vert.spv"
        : "water_surface.vert.spv");
    desc.fragPath = shaderPath("water.frag.spv");
    realisticSurfacePipeline_ = std::make_unique<Pipeline>(ctx.device, desc);
    desc.vertPath = shaderPath(ctx.stereo()
        ? "multiview.cartoon_water_surface.vert.spv"
        : "cartoon_water_surface.vert.spv");
    desc.fragPath = shaderPath("cartoon_water.frag.spv");
    cartoonSurfacePipeline_ = std::make_unique<Pipeline>(ctx.device, desc);
}

void WaterFeature::record(FrameContext& fc) {
    ++frame_;
    for (auto it = surfaces_.begin(); it != surfaces_.end();)
        if (frame_ - it->second.lastFrame > framesInFlight_ + 1) it = surfaces_.erase(it);
        else ++it;
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
        [&](const RetiredBuffer& b) { return b.releaseFrame <= frame_; }), retired_.end());

    std::array<GpuWater, kMaxWaters> packed{};
    std::array<WaterNode*, kMaxWaters> nodes{};
    uint32_t waterCount = 0;

    // Pack every water node into this frame's UBO array.
    for (WaterNode* w : fc.scene.waterNodes()) {
        if (!w->isActiveInHierarchy()) continue;
        if (waterCount >= kMaxWaters) break;  // hard cap (UBO array size)

        glm::vec3 c = glm::vec3(w->worldTransform()[3]);
        nodes[waterCount] = w;
        GpuWater& g = packed[waterCount++];
        g.area = glm::vec4(c.x, c.y, c.z, w->size);
        g.deep = glm::vec4(w->deepColor, w->roughness);
        g.foam = glm::vec4(w->foamColor, w->reflectivity);
        g.waveA = glm::vec4(w->amplitude, w->wavelength, w->waveSpeed, w->choppiness);
        g.detail1 = glm::vec4(w->detailScale, w->detailSpeed, w->detailStrength, w->detailAngle);
        g.detail2 = glm::vec4(w->detail2Scale, w->detail2Speed, w->detail2Strength, w->detail2Angle);
        g.look = glm::vec4(w->fresnelPower, w->specularPower, w->specularIntensity, w->foamThreshold);
        g.misc = glm::vec4(w->warpAmount, w->detailFadeDistance, w->foamIntensity, w->depthColorFalloff);
        g.shoreColor = glm::vec4(w->shallowColor, w->edgeFade);

        const int mode = static_cast<int>(w->shoreMode);
        if (mode == 1) {  // Beach: shoreline direction (inland) + waterline distance + slope.
            const float a = glm::radians(w->shoreAngle);
            g.shoreGeom = glm::vec4(std::cos(a), std::sin(a), w->shoreWaterline, w->shoreSlope);
        } else {          // Lake (and unused for None): centre on the node, radius + slope.
            g.shoreGeom = glm::vec4(c.x, c.z, w->lakeRadius, w->shoreSlope);
        }
        g.shoreTune = glm::vec4(w->foamWidth, w->swashSpeed, w->swashAmount, w->waveFlatten);
        g.shoreMode = glm::vec4(static_cast<float>(mode), w->shoreFoam,
                               static_cast<float>(w->style), float(std::max(fc.extent.height,1u)));
        g.cartoonWave = glm::vec4(w->cartoonWaveScale, w->cartoonWaveSpeed,
                                  w->cartoonWaveAngle, w->cartoonWaveSharpness);
        g.cartoonDetail = glm::vec4(w->cartoonDetailScale, w->cartoonDetailSpeed,
                                    w->cartoonDetailAngle, w->cartoonDetailStrength);
        g.cartoonLook = glm::vec4(w->cartoonColorSteps, w->cartoonColorContrast,
                                  w->cartoonCrestWidth, w->cartoonCrestIntensity);
        g.cartoonShore = glm::vec4(w->cartoonShoreFrequency,
                                   w->cartoonShoreIrregularity,
                                   w->cartoonShoreSharpness,
                                   w->cartoonShoreBands);
        g.localToWorld = w->worldTransform();
        g.dynamics = glm::vec4(static_cast<float>(w->waveType),
            std::clamp(w->waveIntensity,0.0f,3.0f),w->windAngle,std::clamp(w->gustStrength,0.0f,1.0f));
    }
    if (waterCount == 0) return;

    const uint32_t frame = std::min<uint32_t>(fc.frameIndex,
                                              static_cast<uint32_t>(ubos_.size()) - 1);
    ubos_[frame]->write(packed.data(), sizeof(GpuWater) * waterCount);

    auto drawStyle = [&](WaterNode::Style style, Pipeline& planePipeline, Pipeline& surfacePipeline, uint32_t planeCount) {
        Pipeline* bound = nullptr;
        for (uint32_t i = 0; i < waterCount; ++i) {
            if (static_cast<int>(packed[i].shoreMode.z + 0.5f) != static_cast<int>(style)) continue;
            WaterNode& node = *nodes[i];
            SurfaceBuffer* surface = nullptr;
            if (!node.surface.empty()) {
                const uint64_t signature = surfaceSignature(node.surface);
                auto& cached = surfaces_[node.id()];
                if (cached.signature != signature || (!cached.vertices && !cached.invalid)) {
                    if (cached.vertices) retired_.push_back({std::move(cached.vertices), frame_ + framesInFlight_ + 1});
                    std::vector<Vertex> vertices;
                    cached.invalid = !parseSurface(node.surface, vertices);
                    if (cached.invalid) {
                        Log::error("[Water] invalid surface triangles on node ", node.name());
                        cached.count = 0;
                    } else {
                        cached.vertices = std::make_unique<Buffer>(*device_, vertices.size() * sizeof(Vertex),
                            rhi::BufferUsage::Vertex, MemoryUsage::HostVisible);
                        cached.vertices->write(vertices.data(), vertices.size() * sizeof(Vertex));
                        cached.count = static_cast<uint32_t>(vertices.size());
                    }
                    cached.signature = signature;
                }
                cached.lastFrame = frame_;
                if (cached.invalid) continue;
                surface = &cached;
            }
            Pipeline& pipeline = surface ? surfacePipeline : planePipeline;
            if (bound != &pipeline) {
                fc.pass.setPipeline(pipeline);
                fc.pass.setBindGroup(0, *fc.globalSet);
                fc.pass.setBindGroup(1, *sets_[frame]);
                bound = &pipeline;
            }
            Push pc{i, fc.time};
            fc.pass.setPushConstants(&pc, sizeof(Push));
            if (surface) fc.pass.setVertexBuffer(*surface->vertices);
            fc.pass.draw(surface ? surface->count : planeCount);
        }
    };

    drawStyle(WaterNode::Style::Realistic, *realisticPipeline_, *realisticSurfacePipeline_, kGridRes * kGridRes * 6);
    drawStyle(WaterNode::Style::Cartoon, *cartoonPipeline_, *cartoonSurfacePipeline_, kCartoonVertexCount);
}

} // namespace saida
