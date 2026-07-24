#include "nodes/UIColorNode.hpp"
#include <nlohmann/json.hpp>

namespace saida {

void UIColorNode::serialize(nlohmann::json& j, ResourceManager& resources) const {
    UINode::serialize(j, resources);
    j["color"] = {color_.r, color_.g, color_.b, color_.a};
}

void UIColorNode::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    UINode::deserialize(j, resources);
    if (j.contains("color")) {
        auto arr = j["color"];
        color_ = glm::vec4(arr[0], arr[1], arr[2], arr[3]);
    }
}

} // namespace saida
