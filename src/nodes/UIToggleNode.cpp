#include "nodes/UIToggleNode.hpp"
#include <nlohmann/json.hpp>

namespace saida {

void UIToggleNode::serialize(nlohmann::json& j, ResourceManager& resources) const {
    UIInteractableNode::serialize(j, resources);
    j["isOn"] = isOn_;
    j["onColor"] = {onColor_.r, onColor_.g, onColor_.b, onColor_.a};
    j["offColor"] = {offColor_.r, offColor_.g, offColor_.b, offColor_.a};
}

void UIToggleNode::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    UIInteractableNode::deserialize(j, resources);
    isOn_ = j.value("isOn", false);
    
    if (j.contains("onColor")) {
        auto arr = j["onColor"];
        onColor_ = glm::vec4(arr[0], arr[1], arr[2], arr[3]);
    }
    if (j.contains("offColor")) {
        auto arr = j["offColor"];
        offColor_ = glm::vec4(arr[0], arr[1], arr[2], arr[3]);
    }
}

} // namespace saida
