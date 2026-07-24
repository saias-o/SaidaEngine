#include "xr/toolkit/XROrigin.hpp"
#include "xr/toolkit/XRInput.hpp"
#include "xr/toolkit/XRTypes.hpp"

#include <glm/gtc/quaternion.hpp>
#include <cmath>
#include <nlohmann/json.hpp>

namespace saida {

XROrigin::XROrigin() : Node("XROrigin") {
    addToGroup(kXROriginGroup);
}

void XROrigin::teleportTo(const glm::vec3& worldTarget) {
    // Move the rig by the horizontal head→target delta, then drop the floor to the
    // target height. The head pose is world space (already includes this offset),
    // so the delta is what remains to bring the player over the target.
    const glm::vec3 head = XRInput::head().position;
    transform_.position.x += worldTarget.x - head.x;
    transform_.position.z += worldTarget.z - head.z;
    transform_.position.y = worldTarget.y;
}

void XROrigin::snapTurn(float deltaYawDeg) {
    // Rotate the rig position around the head pivot (XZ) so the view turns in place.
    const glm::vec3 head = XRInput::head().position;
    const float rad = glm::radians(deltaYawDeg);
    const float c = std::cos(rad), s = std::sin(rad);
    const glm::vec3 rel = transform_.position - head;
    transform_.position = head + glm::vec3(rel.x * c - rel.z * s, rel.y, rel.x * s + rel.z * c);
    yawDegrees += deltaYawDeg;
}

void XROrigin::serialize(nlohmann::json& j, ResourceManager& resources) const {
    Node::serialize(j, resources);
    j["yawDegrees"] = yawDegrees;
}

void XROrigin::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    Node::deserialize(j, resources);
    if (j.contains("yawDegrees")) yawDegrees = j["yawDegrees"].get<float>();
}

} // namespace saida
