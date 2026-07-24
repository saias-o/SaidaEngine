#pragma once

#include "nodes/UINode.hpp"
#include <string>
#include <glm/glm.hpp>

namespace saida {

class UITextNode : public UINode {
public:
    UITextNode() = default;
    virtual ~UITextNode() = default;

    const std::string& text() const { return text_; }
    void setText(const std::string& t) { text_ = t; }

    float fontSize() const { return fontSize_; }
    void setFontSize(float s) { fontSize_ = s; }

    glm::vec4 color() const { return color_; }
    void setColor(const glm::vec4& c) { color_ = c; }

    const char* typeName() const override { return "UITextNode"; }
    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;

private:
    std::string text_ = "Text";
    float fontSize_ = 16.0f;
    glm::vec4 color_ = glm::vec4(1.0f); // Blanc
};

} // namespace saida
