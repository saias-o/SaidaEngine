#include "fx/ParticleRuntime.hpp"
#include "nodes/ParticleSystemNode.hpp"
#include "scene/ReflectedTypes.hpp"
#include "core/Reflection.hpp"
#include "core/Window.hpp"
#include "graphics/VulkanDevice.hpp"
#include "graphics/Buffer.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

using namespace saida;
namespace {
int checks = 0;
void require(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
void near(glm::vec3 actual, glm::vec3 expected, const char* message) {
    require(std::isfinite(actual.x) && std::isfinite(actual.y) && std::isfinite(actual.z) &&
            glm::length(actual - expected) < .0001f, message);
}
void cpuContract() {
    registerReflectedTypes();
    const auto* type = reflect::TypeRegistry::instance().find("ParticleSystem");
    require(type && !type->findProperty("initialVelocity") && !type->findProperty("useInitialVelocity"),
            "runtime velocity override must not alter authored particle formats");
    ParticleSystemNode node;
    require(!node.useInitialVelocity, "existing emitters keep preset velocity by default");
    near(node.initialVelocity, {}, "default velocity is finite zero");
    node.initialVelocity = {3.f, -12.f, 1.f};
    node.useInitialVelocity = true;
    node.effectClass = ParticleSystemNode::EffectClass::Rain;
    node.applyEffectPreset();
    require(node.useInitialVelocity, "a preset preserves the caller's runtime override");
    near(node.initialVelocity, {3.f, -12.f, 1.f}, "a preset preserves world-space velocity");
}

constexpr uint32_t kCapacity = 16;
struct Snapshot {
    std::array<ParticleRuntime::GpuParticle, kCapacity> particles;
    std::array<uint32_t, kCapacity> alive;
    ParticleRuntime::GpuCounters counters;
    std::array<uint32_t, 4> draw;
};
Snapshot readback(VulkanDevice& device, const ParticleRuntime& runtime, uint32_t parity) {
    Buffer particles(device, sizeof(Snapshot::particles), rhi::BufferUsage::TransferDst, MemoryUsage::HostVisible);
    Buffer alive(device, sizeof(Snapshot::alive), rhi::BufferUsage::TransferDst, MemoryUsage::HostVisible);
    Buffer counters(device, sizeof(ParticleRuntime::GpuCounters), rhi::BufferUsage::TransferDst, MemoryUsage::HostVisible);
    Buffer draw(device, ParticleRuntime::kDrawIndirectCommandSize, rhi::BufferUsage::TransferDst, MemoryUsage::HostVisible);
    device.withSingleTimeEncoder([&](rhi::CommandEncoder& encoder) {
        VkMemoryBarrier before{};
        before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        before.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(encoder.handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
        encoder.copyBufferToBuffer(runtime.particlesBuffer(), particles, particles.size());
        encoder.copyBufferToBuffer(runtime.aliveIndicesBuffer(parity), alive, alive.size());
        encoder.copyBufferToBuffer(runtime.countersBuffer(), counters, counters.size());
        encoder.copyBufferToBuffer(runtime.indirectBuffer(), draw, draw.size());
        VkMemoryBarrier after{};
        after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(encoder.handle(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
    });
    Snapshot out{};
    std::memcpy(out.particles.data(), particles.mapped(), particles.size());
    std::memcpy(out.alive.data(), alive.mapped(), alive.size());
    std::memcpy(&out.counters, counters.mapped(), counters.size());
    std::memcpy(out.draw.data(), draw.mapped(), draw.size());
    std::printf("[particles] snapshot parity=%u read=%u alive=%u dead=%u draw=(%u,%u,%u,%u)\n",
                parity, out.counters.readAliveCount, out.counters.aliveCount, out.counters.deadCount,
                out.draw[0], out.draw[1], out.draw[2], out.draw[3]);
    require(out.counters.aliveCount <= kCapacity, "alive count remains within particle storage");
    require(out.draw[0] == out.counters.aliveCount * 6 &&
                out.draw[1] == (out.counters.aliveCount > 0 ? 1u : 0u) &&
                out.draw[2] == 0 && out.draw[3] == 0,
            "indirect draw follows the compacted alive list");
    return out;
}
const ParticleRuntime::GpuParticle& colored(const Snapshot& snapshot, bool red) {
    for (uint32_t i = 0; i < snapshot.counters.aliveCount; ++i) {
        const uint32_t index = snapshot.alive[i];
        require(index < kCapacity, "alive index remains within particle storage");
        const auto& particle = snapshot.particles[index];
        if (red ? particle.colorA.r > .5f : particle.colorA.b > .5f) return particle;
    }
    throw std::runtime_error("the expected emitter's live particle is absent");
}
glm::vec3 acceleration(const ParticleRuntime::GpuParticle& particle) {
    const glm::vec3 offset = glm::vec3(particle.attractor) - glm::vec3(particle.positionAge);
    return glm::vec3(particle.gravity) + glm::normalize(offset) * particle.attractor.w;
}
ParticleRuntime::GpuEmitter emitter(bool rain) {
    ParticleRuntime::GpuEmitter out{};
    out.positionRadius = {rain ? -1.f : 1.f, 0.f, 0.f, 0.f};
    out.gravityLifetime = {0.f, rain ? -10.f : 5.f, 0.f, 10.f};
    out.colorA = rain ? glm::vec4(1, 0, 0, 1) : glm::vec4(0, 0, 1, 1);
    out.colorB = out.colorA;
    out.params = {0.f, .1f, 1.f, rain ? 0.f : 1.f};
    out.shape = glm::vec4(0.f); // Point: positions and velocities are deterministic.
    out.detail = {0.f, 0.f, 1.f, rain ? 9.f : 1.f};
    out.forces.w = rain ? 2.f : 3.f;
    out.attractor = rain ? glm::vec4(-10, 0, 0, 3) : glm::vec4(0, 10, 0, 4);
    out.initialVelocity = rain ? glm::vec4(3, -2, 1, 1) : glm::vec4(-2, 1, 0, 1);
    return out;
}
void gpuContract() {
    // No visible window or editor is created by the optional GPU contract.
    Window window(64, 64, "Particle runtime contracts", false);
    VulkanDevice device(window);
    ParticleRuntime::Desc desc;
    desc.maxParticles = kCapacity;
    desc.maxEmitters = 2;
    ParticleRuntime runtime(device, desc);
    const auto rain = emitter(true), snow = emitter(false);
    auto* current = runtime.mappedEmitters(0);
    current[0] = rain; current[1] = snow;
    runtime.flushEmitters(0, 2);
    uint32_t parity = 0;
    device.withSingleTimeEncoder([&](rhi::CommandEncoder& encoder) {
        parity = runtime.recordCompute(encoder, 0, 2, 2, 0.f, 1.f);
    });
    auto first = readback(device, runtime, parity);
    require(first.counters.aliveCount == 2, "two emitters spawn into the same GPU batch");
    for (bool red : {true, false}) {
        const auto& particle = colored(first, red);
        const auto& source = red ? rain : snow;
        near(glm::vec3(particle.velocityLifetime), glm::vec3(source.initialVelocity), "initial override is exact, without shape jitter");
        near(glm::vec3(particle.gravity), glm::vec3(source.gravityLifetime), "emission captures the correct emitter gravity");
        near(glm::vec3(particle.attractor), glm::vec3(source.attractor), "emission captures the correct emitter attractor");
        require(particle.attractor.w == source.forces.w, "attractor strength belongs to the source emitter");
        require(particle.renderParams.y == (red ? 1.f : 0.f), "rain stretches along velocity while snow keeps its billboard");
    }
    // Reordered, mutated records must never affect particles already in flight.
    current = runtime.mappedEmitters(1);
    current[0] = snow; current[1] = rain;
    for (int i = 0; i < 2; ++i) {
        current[i].gravityLifetime = {1000, 2000, 3000, 10};
        current[i].attractor = {900, 900, 900, 0};
        current[i].forces.w = 1000;
    }
    runtime.flushEmitters(1, 2);
    auto step = [&](uint32_t frame, uint32_t emitters, float dt, const Snapshot& before) {
        device.withSingleTimeEncoder([&](rhi::CommandEncoder& encoder) {
            parity = runtime.recordCompute(encoder, frame, emitters, 0, dt, 2.f);
        });
        auto after = readback(device, runtime, parity);
        require(after.counters.aliveCount == 2, "both particles survive compaction with no new spawns");
        for (bool red : {true, false}) {
            const auto& prior = colored(before, red);
            const auto& now = colored(after, red);
            const auto expected = glm::vec3(prior.velocityLifetime) + acceleration(prior) * dt;
            near(glm::vec3(now.velocityLifetime), expected, "each particle retains its own forces after emitter changes or removal");
            near(glm::vec3(now.positionAge), glm::vec3(prior.positionAge) + expected * dt, "updated velocity advances finite particle positions");
        }
        return after;
    };
    auto second = step(1, 2, .2f, first);
    step(0, 0, .2f, second); // No emitter records: stored forces still apply.
    device.withSingleTimeEncoder([&](rhi::CommandEncoder& encoder) {
        parity = runtime.recordCompute(encoder, 1, 0, 0, 20.f, 3.f);
    });
    auto expired = readback(device, runtime, parity);
    require(expired.counters.aliveCount == 0 && expired.counters.deadCount == kCapacity,
            "expired particles return every slot to the free list");
    runtime.reset();
    device.withSingleTimeEncoder([&](rhi::CommandEncoder& encoder) {
        parity = runtime.recordCompute(encoder, 0, 0, 0, 0.f, 4.f);
    });
    auto reset = readback(device, runtime, parity);
    require(reset.counters.aliveCount == 0 && reset.counters.deadCount == kCapacity,
            "reset initializes both parity and counters without old particles");
}
} // namespace
int main(int argc, char** argv) {
    try {
        cpuContract();
        if (argc > 1 && std::string(argv[1]) == "--gpu") gpuContract();
        std::printf("[particles] %d checks passed\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[particles] FAIL: %s\n", error.what());
        return 1;
    }
}
