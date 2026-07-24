#pragma once

#include "scene/Node.hpp"
#include "core/Signal.hpp"

namespace saida {

// A flat region the player may teleport onto (Godot/Unity teleport-anchor style).
// It is a passive marker: an XRRayInteractor scans the "xr_teleport_area" group,
// ray-tests each area's top face, and on commit moves the XROrigin. The area
// itself just defines the valid surface (local XZ rectangle) and reports landings
// via the `teleported` signal — no physics body required.
class TeleportArea : public Node {
public:
    TeleportArea();

    glm::vec2 size{2.0f, 2.0f};  // full extent on the local X / Z axes (meters)
    bool active = true;          // disabled areas are ignored by interactors

    // Ray vs the area's top face (local plane y=0, clamped to `size`). Writes the
    // world-space hit point and returns true on a forward hit within maxDistance.
    bool raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDistance,
                 glm::vec3& worldHit) const;

    // Emitted when an interactor lands a teleport on this area (world point).
    Signal<glm::vec3> teleported;

    const char* typeName() const override { return "TeleportArea"; }
    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;
};

} // namespace saida
