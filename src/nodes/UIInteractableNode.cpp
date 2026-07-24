#include "nodes/UIInteractableNode.hpp"
#include <nlohmann/json.hpp>

namespace saida {

void UIInteractableNode::serialize(nlohmann::json& j, ResourceManager& resources) const {
    UINode::serialize(j, resources);
    j["interactable"] = interactable_;
}

void UIInteractableNode::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    UINode::deserialize(j, resources);
    interactable_ = j.value("interactable", true);
    state_ = interactable_ ? State::Normal : State::Disabled;
}

} // namespace saida
