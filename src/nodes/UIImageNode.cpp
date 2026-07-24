#include "nodes/UIImageNode.hpp"
#include <nlohmann/json.hpp>

namespace saida {

void UIImageNode::serialize(nlohmann::json& j, ResourceManager& resources) const {
    UINode::serialize(j, resources);
    if (texture_ != 0) {
        j["texture"] = texture_;
    }
}

void UIImageNode::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    UINode::deserialize(j, resources);
    if (j.contains("texture")) {
        if (j["texture"].is_number_integer()) {
            texture_ = j["texture"].get<AssetID>();
        } else if (j["texture"].is_string()) {
            std::string path = j["texture"].get<std::string>();
            texture_ = resources.getOrRegister(path, AssetType::Texture);
        }
    }
}

} // namespace saida
