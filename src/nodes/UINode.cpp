#include "nodes/UINode.hpp"
#include "nodes/UICanvasNode.hpp"
#include <nlohmann/json.hpp> // To walk up the hierarchy

namespace saida {

bool UINode::isPointInside(float px, float py) const {
    float gx = globalX_;
    float gy = globalY_;
    float drawX = gx - (width_ * pivotX_);
    float drawY = gy - (height_ * pivotY_);
    return px >= drawX && px <= drawX + width_ &&
           py >= drawY && py <= drawY + height_;
}

void UINode::getGlobalRect(float& outX, float& outY, float& outW, float& outH) const {
    outX = globalX_ - (width_ * pivotX_);
    outY = globalY_ - (height_ * pivotY_);
    outW = width_;
    outH = height_;
}

void UINode::markDirty() {
    isDirty_ = true;
    for (auto& child : children()) {
        if (auto* uiChild = dynamic_cast<UINode*>(child.get())) {
            uiChild->markDirty();
        }
    }
}

void UINode::updateTransforms(float parentX, float parentY, float parentW, float parentH) {
    if (isDirty_) {
        float anchoredX = parentX + (parentW * anchorX_);
        float anchoredY = parentY + (parentH * anchorY_);

        globalX_ = anchoredX + x_;
        globalY_ = anchoredY + y_;
        
        transform().position.x = globalX_;
        transform().position.y = globalY_;
        transform().position.z = 0.0f;
        
        isDirty_ = false;
    }
    
    float childAreaX = globalX_ - (width_ * pivotX_);
    float childAreaY = globalY_ - (height_ * pivotY_);

    for (auto& child : children()) {
        if (auto* uiChild = dynamic_cast<UINode*>(child.get())) {
            uiChild->updateTransforms(childAreaX, childAreaY, width_, height_);
        }
    }
}

void UINode::serialize(nlohmann::json& j, ResourceManager& resources) const {
    Node::serialize(j, resources);
    j["x"] = x_;
    j["y"] = y_;
    j["width"] = width_;
    j["height"] = height_;
    j["anchorX"] = anchorX_;
    j["anchorY"] = anchorY_;
    j["pivotX"] = pivotX_;
    j["pivotY"] = pivotY_;
}

void UINode::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    Node::deserialize(j, resources);
    x_ = j.value("x", 0.0f);
    y_ = j.value("y", 0.0f);
    width_ = j.value("width", 100.0f);
    height_ = j.value("height", 100.0f);
    anchorX_ = j.value("anchorX", 0.5f);
    anchorY_ = j.value("anchorY", 0.5f);
    pivotX_ = j.value("pivotX", 0.5f);
    pivotY_ = j.value("pivotY", 0.5f);
}

} // namespace saida
