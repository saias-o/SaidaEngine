#pragma once

#include "core/Camera.hpp"
#include "graphics/Mesh.hpp"

#include <algorithm>
#include <cmath>

namespace saida {

// shader.vert: two sine waves (1 + .35), root weight <=1.2^2,
// and a local (1,0,.43) sway direction. Bound the complete displacement.
inline float foliageWindMargin(glm::vec4 parameters) {
    return parameters.w > 0.f ? std::max(0.f,parameters.z)*2.12f : 0.f;
}

// Test in local space against the light's actual clip volume. Transforming
// the planes rather than a sphere keeps nonuniform scale and shear conservative.
inline bool aabbIntersectsClipVolume(const Aabb& bounds, const glm::mat4& clipFromLocal) {
    const glm::mat4 rows = glm::transpose(clipFromLocal);
    const glm::vec4 planes[] = {rows[3] + rows[0], rows[3] - rows[0],
        rows[3] + rows[1], rows[3] - rows[1], rows[2], rows[3] - rows[2]};
    const glm::vec3 center = bounds.center();
    const glm::vec3 half = bounds.extent() * .5f;
    for (const auto& plane : planes) {
        const float distance = glm::dot(glm::vec3(plane), center) + plane.w;
        const float radius = glm::dot(glm::abs(glm::vec3(plane)), half);
        constexpr float relativeTolerance = 1e-4f;
        if (distance + radius < -relativeTolerance * (1.f + std::abs(distance) + radius)) return false;
    }
    return true;
}

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
                                  const Frustum& frustum, float localPadding = 0.f) {
    constexpr float kMinBoundsRadius = 0.001f;
    constexpr float kUnitCubeBoundsRadius = 0.866f;

    const float maxScale = std::max({
        glm::length(glm::vec3(world[0])),
        glm::length(glm::vec3(world[1])),
        glm::length(glm::vec3(world[2])),
    });
    const float localRadius = glm::length(mesh.bounds().extent()) * 0.5f;
    const float radius =
        ((localRadius > kMinBoundsRadius ? localRadius : kUnitCubeBoundsRadius) + localPadding) *
        maxScale;
    const glm::vec3 center =
        glm::vec3(world * glm::vec4(mesh.bounds().center(), 1.0f));
    return sphereIntersectsFrustum(frustum, center, radius);
}

} // namespace saida
