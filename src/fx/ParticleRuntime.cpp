#include "fx/ParticleRuntime.hpp"

#include "core/Paths.hpp"
#include "graphics/Buffer.hpp"
#include "graphics/ComputePipeline.hpp"

#ifndef SAIDA_RHI_WEBGPU
#include "graphics/VulkanDevice.hpp"
#include "rhi/vulkan/CommandEncoder.hpp"
#endif

#include <array>
#include <algorithm>
#include <stdexcept>

namespace saida {

ParticleRuntime::ParticleRuntime(rhi::Device& device, const Desc& desc)
    : device_(device), desc_(desc) {
    desc_.framesInFlight = std::max(1u, desc_.framesInFlight);
    desc_.maxParticles = std::max(1u, desc_.maxParticles);
    desc_.maxEmitters = std::max(1u, desc_.maxEmitters);
    createRenderResources();
    createComputeResources();
}

ParticleRuntime::~ParticleRuntime() = default;

uint32_t ParticleRuntime::frameIndex(uint32_t frame) const {
    return std::min(frame, desc_.framesInFlight - 1);
}

const rhi::BindGroup& ParticleRuntime::renderSet(uint32_t parity) const {
    return *renderSets_[std::min<uint32_t>(parity, 1u)];
}

void ParticleRuntime::reset() {
    initialized_ = false;
    aliveReadParity_ = 0;
}

ParticleRuntime::GpuEmitter* ParticleRuntime::mappedEmitters(uint32_t frame) const {
    return static_cast<GpuEmitter*>(emitterBuffers_[frameIndex(frame)]->mapped());
}

void ParticleRuntime::flushEmitters(uint32_t frame, uint32_t count) {
    if (count == 0) return;
    const uint32_t clamped = std::min(count, desc_.maxEmitters);
    emitterBuffers_[frameIndex(frame)]->flush(sizeof(GpuEmitter) * clamped);
}

uint32_t ParticleRuntime::recordCompute(rhi::CommandEncoder& encoder, uint32_t frame,
                                        uint32_t emitterCount, uint32_t emitCount,
                                        float dt, float time) {
    if (!initialized_) {
        recordInit(encoder);
        initialized_ = true;
    }

    const uint32_t readParity = aliveReadParity_;
    const uint32_t writeParity = 1u - readParity;
    const uint32_t clampedEmitters = std::min(emitterCount, desc_.maxEmitters);
    const uint32_t clampedEmitCount = std::min(emitCount, desc_.maxParticles);
    ComputePush push{desc_.maxParticles, clampedEmitters, clampedEmitCount, dt, time};
    const rhi::BindGroup& set = computeSet(frame, readParity);

    rhi::ComputePassEncoder cp = encoder.beginComputePass();

    cp.setPipeline(*preparePipeline_);
    cp.setBindGroup(0, set);
    cp.setPushConstants(&push, sizeof(push));
    cp.dispatch(1);

    encoder.storageBarrier();

    cp.setPipeline(*simPipeline_);
    cp.setBindGroup(0, set);
    cp.setPushConstants(&push, sizeof(push));
    cp.dispatch(ComputePipeline::groupCount(desc_.maxParticles, 64));

    encoder.storageBarrier();

    if (clampedEmitCount > 0 && clampedEmitters > 0) {
        cp.setPipeline(*emitPipeline_);
        cp.setBindGroup(0, set);
        cp.setPushConstants(&push, sizeof(push));
        cp.dispatch(ComputePipeline::groupCount(clampedEmitCount, 64));

        encoder.storageBarrier();
    }

    cp.setPipeline(*finalizePipeline_);
    cp.setBindGroup(0, set);
    cp.setPushConstants(&push, sizeof(push));
    cp.dispatch(1);
    cp.end();

    // Sim results -> indirect draw args + vertex-stage particle fetches.
    encoder.computeToIndirectBarrier();

    aliveReadParity_ = writeParity;
    return writeParity;
}

void ParticleRuntime::createRenderResources() {
#ifdef SAIDA_RHI_WEBGPU
    // WebGPU forbids read-write storage in the vertex stage; the render shader
    // only reads these (particle + alive buffers), so mark them read-only.
    auto readOnlyVert = [](uint32_t binding) {
        rhi::webgpu::BindGroupLayoutEntry e{};
        e.binding = binding;
        e.type = rhi::BindingType::StorageBuffer;
        e.visibility = rhi::ShaderStages::Vertex;
        e.readOnlyStorage = true;
        return e;
    };
    renderSetLayout_ = std::make_unique<rhi::BindGroupLayout>(device_,
        std::vector<rhi::webgpu::BindGroupLayoutEntry>{readOnlyVert(0), readOnlyVert(1)});
#else
    renderSetLayout_ = std::make_unique<rhi::BindGroupLayout>(device_,
        std::vector<rhi::BindGroupLayoutEntry>{
            {0, rhi::BindingType::StorageBuffer, rhi::ShaderStages::Vertex},
            {1, rhi::BindingType::StorageBuffer, rhi::ShaderStages::Vertex},
        });
#endif
    renderSets_.resize(2);
}

void ParticleRuntime::createComputeResources() {
#ifdef SAIDA_RHI_WEBGPU
    // Bindings 1 (ReadAlive) and 5 (Emitter) are `readonly` in every compute
    // shader; WebGPU requires the layout access to match the WGSL exactly.
    std::vector<rhi::webgpu::BindGroupLayoutEntry> computeEntries;
    for (uint32_t i = 0; i < 7; ++i) {
        rhi::webgpu::BindGroupLayoutEntry e{};
        e.binding = i;
        e.type = rhi::BindingType::StorageBuffer;
        e.visibility = rhi::ShaderStages::Compute;
        e.readOnlyStorage = (i == 1 || i == 5);
        computeEntries.push_back(e);
    }
#else
    std::vector<rhi::BindGroupLayoutEntry> computeEntries;
    for (uint32_t i = 0; i < 7; ++i)
        computeEntries.push_back({i, rhi::BindingType::StorageBuffer, rhi::ShaderStages::Compute});
#endif
    computeSetLayout_ = std::make_unique<rhi::BindGroupLayout>(device_, computeEntries);

    emitterBuffers_.resize(desc_.framesInFlight);
    computeSets_.resize(desc_.framesInFlight * 2);

    const uint64_t particleSize = sizeof(SimParticle) * desc_.maxParticles;
    const uint64_t indexSize = sizeof(uint32_t) * desc_.maxParticles;
    const uint64_t counterSize = sizeof(GpuCounters);
    const uint64_t emitterSize = sizeof(GpuEmitter) * desc_.maxEmitters;
    simParticleBuffer_ = std::make_unique<Buffer>(device_, particleSize,
        rhi::BufferUsage::Storage, MemoryUsage::GpuOnly);
    for (auto& alive : aliveIndexBuffers_) {
        alive = std::make_unique<Buffer>(device_, indexSize,
            rhi::BufferUsage::Storage, MemoryUsage::GpuOnly);
    }
    deadIndexBuffer_ = std::make_unique<Buffer>(device_, indexSize,
        rhi::BufferUsage::Storage, MemoryUsage::GpuOnly);
    counterBuffer_ = std::make_unique<Buffer>(device_, counterSize,
        rhi::BufferUsage::Storage, MemoryUsage::GpuOnly);
    indirectBuffer_ = std::make_unique<Buffer>(device_, kDrawIndirectCommandSize,
        rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect,
        MemoryUsage::GpuOnly);

    for (uint32_t i = 0; i < desc_.framesInFlight; ++i) {
        emitterBuffers_[i] = std::make_unique<Buffer>(device_, emitterSize,
            rhi::BufferUsage::Storage, MemoryUsage::HostVisible);

        for (uint32_t parity = 0; parity < 2; ++parity) {
            const uint32_t setIndex = i * 2 + parity;
            const uint32_t writeParity = 1u - parity;

            std::vector<rhi::BindGroupEntry> entries(7);
            entries[0] = {0, simParticleBuffer_.get(), 0, particleSize};
            entries[1] = {1, aliveIndexBuffers_[parity].get(), 0, indexSize};
            entries[2] = {2, aliveIndexBuffers_[writeParity].get(), 0, indexSize};
            entries[3] = {3, deadIndexBuffer_.get(), 0, indexSize};
            entries[4] = {4, counterBuffer_.get(), 0, counterSize};
            entries[5] = {5, emitterBuffers_[i].get(), 0, emitterSize};
            entries[6] = {6, indirectBuffer_.get(), 0, kDrawIndirectCommandSize};
            computeSets_[setIndex] = std::make_unique<rhi::BindGroup>(*computeSetLayout_, entries);
        }
    }

    const uint64_t particleSizeRender = sizeof(SimParticle) * desc_.maxParticles;
    const uint64_t indexSizeRender = sizeof(uint32_t) * desc_.maxParticles;
    for (uint32_t parity = 0; parity < 2; ++parity) {
        std::vector<rhi::BindGroupEntry> entries(2);
        entries[0] = {0, simParticleBuffer_.get(), 0, particleSizeRender};
        entries[1] = {1, aliveIndexBuffers_[parity].get(), 0, indexSizeRender};
        renderSets_[parity] = std::make_unique<rhi::BindGroup>(*renderSetLayout_, entries);
    }

#ifdef SAIDA_RHI_WEBGPU
    std::vector<const rhi::BindGroupLayout*> setLayouts = {computeSetLayout_.get()};
    initPipeline_ = std::make_unique<ComputePipeline>(device_, "/shaders/particle_init.comp.wgsl",
        setLayouts, sizeof(ComputePush));
    preparePipeline_ = std::make_unique<ComputePipeline>(device_, "/shaders/particle_prepare.comp.wgsl",
        setLayouts, sizeof(ComputePush));
    emitPipeline_ = std::make_unique<ComputePipeline>(device_, "/shaders/particle_emit.comp.wgsl",
        setLayouts, sizeof(ComputePush));
    simPipeline_ = std::make_unique<ComputePipeline>(device_, "/shaders/particle_sim.comp.wgsl",
        setLayouts, sizeof(ComputePush));
    finalizePipeline_ = std::make_unique<ComputePipeline>(device_, "/shaders/particle_finalize.comp.wgsl",
        setLayouts, sizeof(ComputePush));
#else
    std::vector<rhi::vulkan::BindGroupLayoutRef> setLayouts = {*computeSetLayout_};
    initPipeline_ = std::make_unique<ComputePipeline>(device_, shaderPath("particle_init.comp.spv"),
        setLayouts, sizeof(ComputePush));
    preparePipeline_ = std::make_unique<ComputePipeline>(device_, shaderPath("particle_prepare.comp.spv"),
        setLayouts, sizeof(ComputePush));
    emitPipeline_ = std::make_unique<ComputePipeline>(device_, shaderPath("particle_emit.comp.spv"),
        setLayouts, sizeof(ComputePush));
    simPipeline_ = std::make_unique<ComputePipeline>(device_, shaderPath("particle_sim.comp.spv"),
        setLayouts, sizeof(ComputePush));
    finalizePipeline_ = std::make_unique<ComputePipeline>(device_, shaderPath("particle_finalize.comp.spv"),
        setLayouts, sizeof(ComputePush));
#endif
}

const rhi::BindGroup& ParticleRuntime::computeSet(uint32_t frame, uint32_t readParity) const {
    const uint32_t f = frameIndex(frame);
    return *computeSets_[f * 2 + std::min<uint32_t>(readParity, 1u)];
}

void ParticleRuntime::recordInit(rhi::CommandEncoder& encoder) const {
    ComputePush push{desc_.maxParticles, 0, 0, 0.0f, 0.0f};
    rhi::ComputePassEncoder cp = encoder.beginComputePass();
    cp.setPipeline(*initPipeline_);
    cp.setBindGroup(0, computeSet(0, aliveReadParity_));
    cp.setPushConstants(&push, sizeof(push));
    cp.dispatch(ComputePipeline::groupCount(desc_.maxParticles, 64));
    cp.end();

    encoder.storageBarrier();
}

} // namespace saida
