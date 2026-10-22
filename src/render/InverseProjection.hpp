#pragma once

#include <glm/glm.hpp>

namespace saida {

// The terms of a projection's inverse that rebuild a view-space point from a
// viewport coordinate and its reversed depth, packed as
// shaders/depth_reconstruction.glsl reads them. The ambient occlusion and the
// fog share this one packing so that both rebuild the same surface.
struct InverseProjection {
    glm::vec4 scale{0.0f};    // x invP00, y invP11, z invP23, w invP33
    glm::vec4 depth{0.0f};    // x invP22, y invP32
    glm::vec4 offsets{0.0f};  // xy invP30/31, zw invP20/21: an off-centre (OpenXR) eye
};

inline InverseProjection inverseProjection(const glm::mat4& projection) {
    const glm::mat4 inv = glm::inverse(projection);
    InverseProjection packed;
    packed.scale = glm::vec4(inv[0][0], inv[1][1], inv[2][3], inv[3][3]);
    packed.depth = glm::vec4(inv[2][2], inv[3][2], 0.0f, 0.0f);
    packed.offsets = glm::vec4(inv[3][0], inv[3][1], inv[2][0], inv[2][1]);
    return packed;
}

} // namespace saida
