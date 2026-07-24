#pragma once

#include "scene/Node.hpp"

namespace saida {

// The player rig — a logical control point for where the tracking space sits in
// the world. The engine pushes this node's pose to the OpenXR session each frame,
// which recenters the reference space; head/hands/eyes then come back already in
// world space (no per-node offset, compositor stays consistent). Locomotion =
// move/rotate this node. Place ONE in the scene and add it to nothing special —
// it registers in the "xr_origin" group itself.
class XROrigin : public Node {
public:
    XROrigin();

    // Teleport the player so the head ends up horizontally over `worldTarget`,
    // with the floor at the target's height. Uses the live head pose.
    void teleportTo(const glm::vec3& worldTarget);

    // Snap-turn the rig by `deltaYawDeg`, pivoting around the player's head so the
    // view rotates in place rather than orbiting the origin.
    void snapTurn(float deltaYawDeg);

    float yawDegrees = 0.0f;  // rig yaw about +Y (world)

    const char* typeName() const override { return "XROrigin"; }
    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;
};

} // namespace saida
