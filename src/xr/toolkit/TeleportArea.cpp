#include "xr/toolkit/TeleportArea.hpp"
#include "xr/toolkit/XRTypes.hpp"

#include <glm/glm.hpp>
#include <cmath>
#include <nlohmann/json.hpp>

namespace saida {

TeleportArea::TeleportArea() : Node("TeleportArea") {
    addToGroup(kXRTeleportAreaGroup);
}

bool TeleportArea::raycast(const glm::vec3& origin, const glm::vec3& dir,
                           float maxDistance, glm::vec3& worldHit) const {
    if (!active) return false;

    // Work in the area's local frame so the test is a simple plane-y=0 + rectangle.
    const glm::mat4 toLocal = glm::inverse(worldTransform_);
    const glm::vec3 lo = glm::vec3(toLocal * glm::vec4(origin, 1.0f));
    const glm::vec3 ld = glm::vec3(toLocal * glm::vec4(dir, 0.0f));

    if (std::abs(ld.y) < 1e-6f) return false;       // parallel to the surface
    const float t = -lo.y / ld.y;
    if (t < 0.0f) return false;                     // behind the ray origin

    const glm::vec3 localHit = lo + t * ld;
    if (std::abs(localHit.x) > size.x * 0.5f || std::abs(localHit.z) > size.y * 0.5f)
        return false;                               // outside the rectangle

    worldHit = glm::vec3(worldTransform_ * glm::vec4(localHit, 1.0f));
    return glm::distance(origin, worldHit) <= maxDistance;
}

void TeleportArea::serialize(nlohmann::json& j, ResourceManager& resources) const {
    Node::serialize(j, resources);
    j["sizeX"] = size.x;
    j["sizeY"] = size.y;
    j["active"] = active;
}

void TeleportArea::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    Node::deserialize(j, resources);
    if (j.contains("sizeX")) size.x = j["sizeX"].get<float>();
    if (j.contains("sizeY")) size.y = j["sizeY"].get<float>();
    if (j.contains("active")) active = j["active"].get<bool>();
}

} // namespace saida
