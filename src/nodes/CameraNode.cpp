#include "nodes/CameraNode.hpp"

#include <nlohmann/json.hpp>

namespace saida {

CameraNode::CameraNode() : Node("Camera") {
    // Cameras are located by the director (and by the character for camera-relative
    // movement) through this group — never by name.
    addToGroup("camera");
}

CameraNode::CameraNode(std::string name) : Node(std::move(name)) {
    addToGroup("camera");
}

void CameraNode::serialize(nlohmann::json& j, ResourceManager& resources) const {
    Node::serialize(j, resources);
    j["fovDegrees"] = fovDegrees;
    j["nearZ"] = nearZ;
    j["farZ"] = farZ;
    j["priority"] = priority;
    j["active"] = active;
}

void CameraNode::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    Node::deserialize(j, resources);
    if (j.contains("fovDegrees")) fovDegrees = j["fovDegrees"].get<float>();
    if (j.contains("nearZ")) nearZ = j["nearZ"].get<float>();
    if (j.contains("farZ")) farZ = j["farZ"].get<float>();
    if (j.contains("priority")) priority = j["priority"].get<int>();
    if (j.contains("active")) active = j["active"].get<bool>();
}

} // namespace saida
