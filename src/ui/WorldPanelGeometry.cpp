#include "ui/WorldPanelGeometry.hpp"

#include <cmath>

namespace saida {

bool raycastWorldPanel(const glm::mat4& worldTransform,
                       float worldWidth, float worldHeight,
                       uint32_t pixelWidth, uint32_t pixelHeight,
                       const glm::vec3& origin, const glm::vec3& direction,
                       WorldPanelHit& out) {
    if (pixelWidth == 0 || pixelHeight == 0 || worldWidth <= 0.0f || worldHeight <= 0.0f) {
        return false;
    }

    const glm::mat4 invWorld = glm::inverse(worldTransform);
    const glm::vec3 localOrigin = glm::vec3(invWorld * glm::vec4(origin, 1.0f));
    const glm::vec3 localDirection = glm::normalize(glm::vec3(invWorld * glm::vec4(direction, 0.0f)));
    if (std::abs(localDirection.z) < 1e-5f) return false;  // parallel to the panel

    const float t = -localOrigin.z / localDirection.z;
    if (t < 0.0f) return false;  // panel is behind the ray

    const glm::vec3 hit = localOrigin + localDirection * t;
    const float halfW = worldWidth * 0.5f;
    const float halfH = worldHeight * 0.5f;
    if (hit.x < -halfW || hit.x > halfW || hit.y < -halfH || hit.y > halfH) {
        return false;  // outside the panel bounds
    }

    const glm::vec3 worldHit = glm::vec3(worldTransform * glm::vec4(hit, 1.0f));
    out.distance = glm::length(worldHit - origin);
    out.local = {
        (hit.x / worldWidth + 0.5f) * static_cast<float>(pixelWidth),
        (0.5f - hit.y / worldHeight) * static_cast<float>(pixelHeight),
    };
    return true;
}

} // namespace saida
