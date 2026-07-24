#pragma once

#include "scene/Node.hpp"

namespace saida {

// Root of a screen-space UI tree. Hit-testing and interaction live in the
// single canonical path, UIInteractionSystem; this node only carries the
// design-space dimensions and its UINode children.
class UICanvasNode : public Node {
public:
    UICanvasNode() = default;
    virtual ~UICanvasNode() = default;

    // Canvas dimensions (often the screen/window size)
    float width() const { return width_; }
    float height() const { return height_; }
    void setSize(float w, float h) { width_ = w; height_ = h; }

    const char* typeName() const override { return "UICanvasNode"; }
    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;

private:
    float width_ = 1920.0f;
    float height_ = 1080.0f;
};

} // namespace saida
