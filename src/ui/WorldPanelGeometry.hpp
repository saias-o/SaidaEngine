#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace saida {

struct WorldPanelHit {
    glm::vec2 local{0.0f};   // pixel-space hit: x in [0,pixelWidth], y in [0,pixelHeight], origin top-left
    float distance = 0.0f;   // world-space distance from the ray origin to the hit
};

// Intersect a world ray with a flat panel that lies on its own local z=0 plane,
// sized worldWidth × worldHeight and centered on the local origin, then map the
// hit into the panel's pixel space (top-left origin, y pointing down). This is
// the pure geometry behind a World-Space UI panel, independent of any GPU or UI
// backend so it can be reasoned about and tested on its own.
//
// Returns false when the panel is degenerate (zero pixel or world extent), the
// ray is parallel to the plane, the intersection is behind the ray origin, or
// the hit falls outside the panel bounds.
bool raycastWorldPanel(const glm::mat4& worldTransform,
                       float worldWidth, float worldHeight,
                       uint32_t pixelWidth, uint32_t pixelHeight,
                       const glm::vec3& origin, const glm::vec3& direction,
                       WorldPanelHit& out);

} // namespace saida
