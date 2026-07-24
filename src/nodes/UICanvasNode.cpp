#include "nodes/UICanvasNode.hpp"
#include <nlohmann/json.hpp>

namespace saida {

void UICanvasNode::serialize(nlohmann::json& j, ResourceManager& resources) const {
    Node::serialize(j, resources);
    j["width"] = width_;
    j["height"] = height_;
}

void UICanvasNode::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    Node::deserialize(j, resources);
    width_ = j.value("width", 1920.0f);
    height_ = j.value("height", 1080.0f);
}

} // namespace saida
