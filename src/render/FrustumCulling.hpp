#pragma once

#include "core/Camera.hpp"
#include "graphics/Mesh.hpp"

#include <algorithm>

namespace saida {

inline bool sphereIntersectsFrustum(const Frustum& frustum,
                                    const glm::vec3& center,
                                    float radius) {
    for (const glm::vec4& plane : frustum.planes) {
        if (glm::dot(glm::vec3(plane), center) + plane.w < -radius) {
            return false;
        }
    }
    return true;
}

inline bool meshIntersectsFrustum(const Mesh& mesh, const glm::mat4& world,
                                  const Frustum& frustum) {
    constexpr float kMinBoundsRadius = 0.001f;
    constexpr float kUnitCubeBoundsRadius = 0.866f;

    const float maxScale = std::max({
        glm::length(glm::vec3(world[0])),
        glm::length(glm::vec3(world[1])),
        glm::length(glm::vec3(world[2])),
    });
    const float localRadius = glm::length(mesh.bounds().extent()) * 0.5f;
    const float radius =
        (localRadius > kMinBoundsRadius ? localRadius : kUnitCubeBoundsRadius) *
        maxScale;
    const glm::vec3 center =
        glm::vec3(world * glm::vec4(mesh.bounds().center(), 1.0f));
    return sphereIntersectsFrustum(frustum, center, radius);
}

} // namespace saida
