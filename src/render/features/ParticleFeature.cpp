#include "render/features/ParticleFeature.hpp"

#include "core/Camera.hpp"
#include "core/Paths.hpp"

#ifndef SAIDA_RHI_WEBGPU
#include "graphics/VulkanDevice.hpp"
#include "rhi/vulkan/Format.hpp"
#endif
#include "scene/Scene.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace saida {

namespace {

uint32_t hashPtr(const void* p) {
    uint64_t v = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p));
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33;
    return static_cast<uint32_t>(v) | 1u;
}

bool sphereInFrustum(const Frustum& frustum, const glm::vec3& center, float radius) {
    for (const glm::vec4& plane : frustum.planes) {
        if (glm::dot(glm::vec3(plane), center) + plane.w < -radius) return false;
    }
    return true;
}

Frustum frustumFromViewProjection(const glm::mat4& view, const glm::mat4& projection) {
    Frustum f;
    glm::mat4 m = projection * view;
    glm::vec4 row0 = glm::vec4(m[0][0], m[1][0], m[2][0], m[3][0]);
    glm::vec4 row1 = glm::vec4(m[0][1], m[1][1], m[2][1], m[3][1]);
    glm::vec4 row2 = glm::vec4(m[0][2], m[1][2], m[2][2], m[3][2]);
    glm::vec4 row3 = glm::vec4(m[0][3], m[1][3], m[2][3], m[3][3]);
    f.planes[0] = row3 + row0;
    f.planes[1] = row3 - row0;
    f.planes[2] = row3 + row1;
    f.planes[3] = row3 - row1;
    f.planes[4] = row2;
    f.planes[5] = row3 - row2;
    for (glm::vec4& plane : f.planes) {
        const float len = glm::length(glm::vec3(plane));
        if (len > 0.0f) plane /= len;
    }
    return f;
}

bool sphereInAnyFrustum(const std::array<Frustum, 2>& frustums, uint32_t count,
                        const glm::vec3& center, float radius) {
    for (uint32_t i = 0; i < count; ++i) {
        if (sphereInFrustum(frustums[i], center, radius)) return true;
    }
    return false;
}

float emitterCullRadius(const ParticleSystemNode& emitter) {
    const float travel = std::max(0.0f, emitter.startSpeed) * std::max(0.0f, emitter.lifetime);
    const float gravityTravel = 0.5f * glm::length(emitter.gravity) * emitter.lifetime * emitter.lifetime;
    const float shapeRadius = std::max(emitter.radius, glm::length(emitter.boxExtents));
    return std::max(0.1f, shapeRadius + travel + gravityTravel + emitter.startSize * emitter.stretch * 2.0f);
}

ParticleRuntime::GpuEmitter makeGpuEmitter(const ParticleSystemNode& emitter,
                                           uint32_t spawnCount, uint32_t spawnOffset) {
    ParticleRuntime::GpuEmitter gpu{};
    gpu.positionRadius = glm::vec4(glm::vec3(emitter.worldTransform()[3]),
                                   std::max(0.0f, emitter.radius));
    gpu.gravityLifetime = glm::vec4(emitter.gravity, std::max(0.02f, emitter.lifetime));
    gpu.colorA = emitter.startColor;
    gpu.colorB = emitter.endColor;
    gpu.colorA.r *= emitter.emissive;
    gpu.colorA.g *= emitter.emissive;
    gpu.colorA.b *= emitter.emissive;
    gpu.colorB.r *= emitter.emissive;
    gpu.colorB.g *= emitter.emissive;
    gpu.colorB.b *= emitter.emissive;
    gpu.params = glm::vec4(std::max(0.0f, emitter.startSpeed),
                           std::max(0.001f, emitter.startSize),
                           static_cast<float>(spawnCount),
                           static_cast<float>(spawnOffset));
    gpu.shape = glm::vec4(static_cast<float>(emitter.shape),
                          emitter.boxExtents.x, emitter.boxExtents.y, emitter.boxExtents.z);
    gpu.detail = glm::vec4(glm::radians(glm::clamp(emitter.coneAngle, 1.0f, 89.0f)),
                           std::max(0.0f, emitter.ringThickness),
                           std::max(0.0f, emitter.endSizeScale),
                           std::max(1.0f, emitter.stretch));
    gpu.forces = glm::vec4(std::max(0.0f, emitter.drag),
                           std::max(0.0f, emitter.noiseStrength),
                           std::max(0.001f, emitter.noiseFrequency),
                           emitter.attractorStrength);
    gpu.attractor = glm::vec4(emitter.attractorPosition,
                              static_cast<float>(emitter.effectClass));
    return gpu;
}

} // namespace

ParticleFeature::~ParticleFeature() {
    additiveRuntime_.reset();
    alphaRuntime_.reset();
}

void ParticleFeature::createPipelines(const RenderContext& ctx) {
    device_ = &ctx.device;
    budget_ = particleQualityBudget(ctx.device.capabilities().tier);
    const uint32_t frames = std::max(ctx.framesInFlight, kFramesInFlightFallback);
    ParticleRuntime::Desc runtimeDesc{};
    runtimeDesc.framesInFlight = frames;
    runtimeDesc.maxParticles = budget_.maxGpuParticles;
    runtimeDesc.maxEmitters = budget_.maxEmitters;
    alphaRuntime_ = std::make_unique<ParticleRuntime>(ctx.device, runtimeDesc);
    additiveRuntime_ = std::make_unique<ParticleRuntime>(ctx.device, runtimeDesc);

    Pipeline::Desc desc;
#ifdef SAIDA_RHI_WEBGPU
    desc.vertPath = "/shaders/particle_render.vert.wgsl";
    desc.fragPath = "/shaders/particle_render.frag.wgsl";
#else
    const char* vert = ctx.stereo() ? "multiview.particle_render.vert.spv" : "particle_render.vert.spv";
    desc.vertPath = shaderPath(vert);
    desc.fragPath = shaderPath("particle_render.frag.spv");
#endif
    desc.colorFormats = {ctx.colorFormat};
    desc.depthFormat = ctx.depthFormat;
    desc.bindGroupLayouts = {&ctx.globalSetLayout, &alphaRuntime_->renderSetLayout()};
    desc.samples = ctx.samples;
    desc.vertexInput = false;
    desc.depthWrite = false;
    desc.cullMode = rhi::CullMode::None;
    desc.pushConstantSize = sizeof(Push);
    desc.viewMask = ctx.viewMask;

    desc.blendMode = rhi::BlendMode::Alpha;
    alphaPipeline_ = std::make_unique<Pipeline>(ctx.device, desc);

    desc.blendMode = rhi::BlendMode::Additive;
    additivePipeline_ = std::make_unique<Pipeline>(ctx.device, desc);
}

namespace {
glm::vec3 cameraPositionFrom(bool stereo, const Camera* camera,
                             const std::vector<EyeRenderInfo>* eyes) {
    if (!stereo && camera) return camera->position;
    if (eyes && !eyes->empty()) return (*eyes)[0].eyePosition;
    return glm::vec3(0.0f);
}
} // namespace

void ParticleFeature::recordPrePass(const PrePassContext& pc) {
    alphaBatch_ = BatchDraw{};
    additiveBatch_ = BatchDraw{};

    const auto& emitters = pc.scene.particleSystems();
    if (emitters.empty() || !alphaRuntime_ || !additiveRuntime_ || !alphaPipeline_ || !additivePipeline_) {
        if (alphaRuntime_) alphaRuntime_->reset();
        if (additiveRuntime_) additiveRuntime_->reset();
        return;
    }

    ++recordSerial_;
    const glm::vec3 cameraPosition = cameraPositionFrom(pc.stereo, pc.camera, pc.eyes);
    std::array<Frustum, 2> frustums{};
    uint32_t frustumCount = 0;
    if (!pc.stereo && pc.camera) {
        frustums[0] = pc.camera->getFrustum();
        frustumCount = 1;
    } else if (pc.eyes) {
        frustumCount = std::min<uint32_t>(2u, static_cast<uint32_t>(pc.eyes->size()));
        for (uint32_t i = 0; i < frustumCount; ++i) {
            const EyeRenderInfo& eye = (*pc.eyes)[i];
            frustums[i] = frustumFromViewProjection(eye.view, eye.projection);
        }
    }

    for (ParticleSystemNode* emitter : emitters) {
        if (!emitter || !emitter->isActiveInHierarchy()) continue;

        EmitterState& state = states_[emitter];
        state.lastSeenSerial = recordSerial_;
        if (state.effectRevision != emitter->effectRevision()) {
            state = EmitterState{};
            state.lastSeenSerial = recordSerial_;
            state.effectRevision = emitter->effectRevision();
        }
        if (state.seed == 1) state.seed = hashPtr(emitter);
        state.visibleThisFrame = true;
        state.spawnThisFrame = 0;
        state.simDtThisFrame = 0.0f;

        float dt = 0.0f;
        if (state.lastTime >= 0.0f) {
            dt = glm::clamp(pc.time - state.lastTime, 0.0f, 0.1f);
        }
        state.lastTime = pc.time;
        if (emitter->playing) {
            state.emittedFinished = false;
        }

        const uint32_t capacity = particleEmitterCapacity(*emitter, budget_);

        const glm::vec3 emitterPosition = glm::vec3(emitter->worldTransform()[3]);
        if (frustumCount > 0 &&
            !sphereInAnyFrustum(frustums, frustumCount, emitterPosition, emitterCullRadius(*emitter))) {
            state.visibleThisFrame = false;
            continue;
        }

        float simDt = dt;
        const glm::vec3 delta = emitterPosition - cameraPosition;
        const float farDistance2 = budget_.farUpdateDistance * budget_.farUpdateDistance;
        const bool farEmitter = glm::dot(delta, delta) > farDistance2;
        if (farEmitter && budget_.farUpdateInterval > 0.0f && dt > 0.0f) {
            state.updateAccumulator += dt;
            if (state.updateAccumulator < budget_.farUpdateInterval) {
                simDt = 0.0f;
            } else {
                simDt = glm::clamp(state.updateAccumulator, 0.0f, 0.25f);
                state.updateAccumulator = 0.0f;
            }
        } else {
            state.updateAccumulator = 0.0f;
        }
        state.simDtThisFrame = simDt;

        const uint32_t bursts = emitter->consumeBurstCount();
        uint32_t toSpawn = 0;
        if (bursts > 0) {
            const uint32_t configuredBurst = static_cast<uint32_t>(std::max(0, emitter->burstCount));
            const uint32_t burstSize = configuredBurst > 0 ? configuredBurst : std::max(1u, capacity / 8u);
            toSpawn += bursts * burstSize;
        }

        if (emitter->playing && emitter->looping && emitter->spawnRate > 0.0f && simDt > 0.0f) {
            state.spawnAccumulator += emitter->spawnRate * simDt;
            const uint32_t continuousSpawn = static_cast<uint32_t>(std::floor(state.spawnAccumulator));
            state.spawnAccumulator -= static_cast<float>(continuousSpawn);
            toSpawn += continuousSpawn;
        }

        state.spawnThisFrame = std::min(toSpawn, capacity);
        if (state.spawnThisFrame > 0) {
            state.timeSinceLastEmit = 0.0f;
            state.emittedFinished = false;
        } else {
            state.timeSinceLastEmit += simDt;
        }

        if (!emitter->playing && state.timeSinceLastEmit >= std::max(0.02f, emitter->lifetime) &&
            !state.emittedFinished) {
            emitter->finished.emit();
            state.emittedFinished = true;
        }
    }

    for (auto it = states_.begin(); it != states_.end();) {
        if (it->second.lastSeenSerial != recordSerial_) it = states_.erase(it);
        else ++it;
    }

    const uint32_t frame = std::min<uint32_t>(pc.frameIndex, alphaRuntime_->framesInFlight() - 1);
    // Records the sim compute for one blend mode and stashes the draw for the
    // in-pass record() below. Compute MUST be dispatched here (pre-pass), never
    // inside the scene render pass.
    auto runBatch = [&](ParticleSystemNode::BlendMode mode, ParticleRuntime& runtime,
                        Pipeline& pipeline, BatchDraw& batch) {
        ParticleRuntime::GpuEmitter* gpuEmitters = runtime.mappedEmitters(frame);
        if (!gpuEmitters) return;

        uint32_t emitterCount = 0;
        uint32_t emitCount = 0;
        float maxDt = 0.0f;
        bool hasEmitterForMode = false;

        for (ParticleSystemNode* emitter : emitters) {
            if (!emitter || emitter->blendMode != mode || !emitter->isActiveInHierarchy()) continue;
            auto it = states_.find(emitter);
            if (it == states_.end() || !it->second.visibleThisFrame) continue;

            hasEmitterForMode = true;
            if (emitterCount >= budget_.maxEmitters || emitCount >= runtime.maxParticles()) continue;

            EmitterState& state = it->second;
            const uint32_t freeBudget = runtime.maxParticles() - emitCount;
            const uint32_t spawnCount = std::min(state.spawnThisFrame, freeBudget);
            gpuEmitters[emitterCount++] = makeGpuEmitter(*emitter, spawnCount, emitCount);
            emitCount += spawnCount;
            maxDt = std::max(maxDt, state.simDtThisFrame);
        }

        if (!hasEmitterForMode) {
            runtime.reset();
            return;
        }

        runtime.flushEmitters(frame, emitterCount);
        batch.parity = runtime.recordCompute(pc.encoder, frame, emitterCount, emitCount, maxDt, pc.time);
        batch.pipeline = &pipeline;
        batch.runtime = &runtime;
        batch.draw = true;
    };

    runBatch(ParticleSystemNode::BlendMode::Alpha, *alphaRuntime_, *alphaPipeline_, alphaBatch_);
    runBatch(ParticleSystemNode::BlendMode::Additive, *additiveRuntime_, *additivePipeline_, additiveBatch_);
}

void ParticleFeature::record(FrameContext& fc) {
    // Draw only: the sim compute already ran in recordPrePass (WebGPU/Vulkan
    // forbid compute inside a render pass). Parity/pipeline come from the batch.
    auto drawBatch = [&](const BatchDraw& batch) {
        if (!batch.draw || !batch.pipeline || !batch.runtime) return;
        fc.pass.setPipeline(*batch.pipeline);
        fc.pass.setBindGroup(0, *fc.globalSet);
        fc.pass.setBindGroup(1, batch.runtime->renderSet(batch.parity));
        Push push{};
        fc.pass.setPushConstants(&push, sizeof(Push));
        fc.pass.drawIndirect(batch.runtime->indirectBuffer(), 0, 1,
                             ParticleRuntime::kDrawIndirectCommandSize);
    };
    drawBatch(alphaBatch_);
    drawBatch(additiveBatch_);
}

} // namespace saida
