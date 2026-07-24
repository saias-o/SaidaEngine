#pragma once

#include "nodes/UINode.hpp"
#include "graphics/ResourceManager.hpp"

namespace saida {

class UIImageNode : public UINode {
public:
    UIImageNode() = default;
    virtual ~UIImageNode() = default;

    AssetID texture() const { return texture_; }
    void setTexture(AssetID tex) { texture_ = tex; }

    const char* typeName() const override { return "UIImageNode"; }
    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;

private:
    AssetID texture_ = 0;
};

} // namespace saida
