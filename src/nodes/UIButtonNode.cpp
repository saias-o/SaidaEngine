#include "nodes/UIButtonNode.hpp"
#include <nlohmann/json.hpp>

namespace saida {

void UIButtonNode::serialize(nlohmann::json& j, ResourceManager& resources) const {
    UIInteractableNode::serialize(j, resources);
    j["normalColor"] = {normalColor_.r, normalColor_.g, normalColor_.b, normalColor_.a};
    j["hoverColor"] = {hoverColor_.r, hoverColor_.g, hoverColor_.b, hoverColor_.a};
    j["pressedColor"] = {pressedColor_.r, pressedColor_.g, pressedColor_.b, pressedColor_.a};
}

void UIButtonNode::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    UIInteractableNode::deserialize(j, resources);
    
    if (j.contains("normalColor")) {
        auto arr = j["normalColor"];
        normalColor_ = glm::vec4(arr[0], arr[1], arr[2], arr[3]);
    }
    if (j.contains("hoverColor")) {
        auto arr = j["hoverColor"];
        hoverColor_ = glm::vec4(arr[0], arr[1], arr[2], arr[3]);
    }
    if (j.contains("pressedColor")) {
        auto arr = j["pressedColor"];
        pressedColor_ = glm::vec4(arr[0], arr[1], arr[2], arr[3]);
    }
}

} // namespace saida
