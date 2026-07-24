#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>

namespace saida::gpu_driven {

// Must stay in lockstep with shaders/culling.comp.
enum class CullingBinding : uint32_t {
    Instances = 0,
    OriginalDrawCommands = 1,
    DrawCount = 2,
    CulledDrawCommands = 3,
};

constexpr uint32_t binding(CullingBinding value) {
    return static_cast<uint32_t>(value);
}

// Explicit padding keeps the std430/std140 layout stable.
struct alignas(16) InstanceData {
    glm::mat4 model{1.0f};
    glm::vec4 boundingSphere{0.0f};  // local center xyz, local radius w
    uint32_t materialIndex = 0;
    int32_t boneOffset = -1;
    uint32_t pad[2]{};
};

struct DrawIndexedIndirectCommand {
    uint32_t indexCount = 0;
    uint32_t instanceCount = 0;
    uint32_t firstIndex = 0;
    int32_t vertexOffset = 0;
    uint32_t firstInstance = 0;
};

struct CullingPushConstants {
    glm::vec4 frustumPlanes[6]{};
    uint32_t instanceCount = 0;
};

static_assert(alignof(InstanceData) == 16);
static_assert(sizeof(InstanceData) == 96);
static_assert(offsetof(InstanceData, boundingSphere) == 64);
static_assert(offsetof(InstanceData, materialIndex) == 80);
static_assert(offsetof(InstanceData, boneOffset) == 84);
static_assert(sizeof(DrawIndexedIndirectCommand) == 20);
static_assert(offsetof(CullingPushConstants, instanceCount) == 96);

} // namespace saida::gpu_driven
