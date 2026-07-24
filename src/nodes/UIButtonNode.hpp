#pragma once

#include "nodes/UIInteractableNode.hpp"
#include <glm/glm.hpp>

namespace saida {

class UIButtonNode : public UIInteractableNode {
public:
    UIButtonNode() = default;
    virtual ~UIButtonNode() = default;

    glm::vec4 normalColor() const { return normalColor_; }
    glm::vec4 hoverColor() const { return hoverColor_; }
    glm::vec4 pressedColor() const { return pressedColor_; }
    
    void setColors(const glm::vec4& normal, const glm::vec4& hover, const glm::vec4& pressed) {
        normalColor_ = normal;
        hoverColor_ = hover;
        pressedColor_ = pressed;
    }

    // Helper to get the current color based on state
    glm::vec4 currentColor() const {
        if (!interactable_) return normalColor_ * 0.5f; // Darkened if disabled
        switch (state_) {
            case State::Hover: return hoverColor_;
            case State::Pressed: return pressedColor_;
            default: return normalColor_;
        }
    }

    const char* typeName() const override { return "UIButtonNode"; }
    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;

private:
    glm::vec4 normalColor_ = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
    glm::vec4 hoverColor_ = glm::vec4(0.9f, 0.9f, 0.9f, 1.0f);
    glm::vec4 pressedColor_ = glm::vec4(0.6f, 0.6f, 0.6f, 1.0f);
};

} // namespace saida
