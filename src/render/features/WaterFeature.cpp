#include "render/features/WaterFeature.hpp"

#include "core/Paths.hpp"
#include "graphics/Buffer.hpp"
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
#include <stdexcept>
#include <vector>

namespace saida {

WaterFeature::~WaterFeature() = default;

void WaterFeature::createPipelines(const RenderContext& ctx) {
    device_ = &ctx.device;
    const uint32_t frames = std::max(1u, ctx.framesInFlight);

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
}

void WaterFeature::record(FrameContext& fc) {
    std::array<GpuWater, kMaxWaters> packed{};
    uint32_t waterCount = 0;

    // Pack every water node into this frame's UBO array.
    for (WaterNode* w : fc.scene.waterNodes()) {
        if (!w->isActiveInHierarchy()) continue;
        if (waterCount >= kMaxWaters) break;  // hard cap (UBO array size)

        glm::vec3 c = glm::vec3(w->worldTransform()[3]);
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
                               static_cast<float>(w->style), 0.0f);
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
    }
    if (waterCount == 0) return;

    const uint32_t frame = std::min<uint32_t>(fc.frameIndex,
                                              static_cast<uint32_t>(ubos_.size()) - 1);
    ubos_[frame]->write(packed.data(), sizeof(GpuWater) * waterCount);

    auto drawStyle = [&](WaterNode::Style style, Pipeline& pipeline, uint32_t vertexCount) {
        bool bound = false;
        for (uint32_t i = 0; i < waterCount; ++i) {
            if (static_cast<int>(packed[i].shoreMode.z + 0.5f) != static_cast<int>(style)) continue;
            if (!bound) {
                fc.pass.setPipeline(pipeline);
                fc.pass.setBindGroup(0, *fc.globalSet);
                fc.pass.setBindGroup(1, *sets_[frame]);
                bound = true;
            }
            Push pc{i, fc.time};
            fc.pass.setPushConstants(&pc, sizeof(Push));
            fc.pass.draw(vertexCount);
        }
    };

    drawStyle(WaterNode::Style::Realistic, *realisticPipeline_, kGridRes * kGridRes * 6);
    drawStyle(WaterNode::Style::Cartoon, *cartoonPipeline_, kCartoonVertexCount);
}

} // namespace saida
