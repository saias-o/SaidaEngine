#pragma once

#include "nodes/UINode.hpp"
#include <glm/glm.hpp>

namespace saida {

class UIColorNode : public UINode {
public:
    UIColorNode() = default;
    virtual ~UIColorNode() = default;

    glm::vec4 color() const { return color_; }
    void setColor(const glm::vec4& c) { color_ = c; }

    const char* typeName() const override { return "UIColorNode"; }
    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;

private:
    glm::vec4 color_ = glm::vec4(1.0f); // Opaque white by default
};

} // namespace saida
