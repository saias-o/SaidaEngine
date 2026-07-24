#pragma once

#include "scene/Node.hpp"

namespace saida {

class UINode : public Node {
public:
    UINode() = default;
    virtual ~UINode() = default;

    float x() const { return x_; }
    float y() const { return y_; }
    float width() const { return width_; }
    float height() const { return height_; }
    float anchorX() const { return anchorX_; }
    float anchorY() const { return anchorY_; }
    float pivotX() const { return pivotX_; }
    float pivotY() const { return pivotY_; }

    void setPosition(float x, float y) { x_ = x; y_ = y; markDirty(); }
    void setSize(float width, float height) { width_ = width; height_ = height; }
    void setAnchor(float ax, float ay) { anchorX_ = ax; anchorY_ = ay; markDirty(); }
    void setPivot(float px, float py) { pivotX_ = px; pivotY_ = py; markDirty(); }

    void markDirty();
    void updateTransforms(float parentX, float parentY, float parentW, float parentH);
    
    float globalX() const { return globalX_; }
    float globalY() const { return globalY_; }

    bool isPointInside(float px, float py) const;
    void getGlobalRect(float& outX, float& outY, float& outW, float& outH) const;

    const char* typeName() const override { return "UINode"; }
    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;

protected:
    float x_ = 0.0f;
    float y_ = 0.0f;
    float width_ = 100.0f;
    float height_ = 100.0f;
    float anchorX_ = 0.5f; // 0=left, 1=right
    float anchorY_ = 0.5f; // 0=top, 1=bottom
    float pivotX_ = 0.5f;
    float pivotY_ = 0.5f;

    bool isDirty_ = true;
    float globalX_ = 0.0f;
    float globalY_ = 0.0f;
};

} // namespace saida
