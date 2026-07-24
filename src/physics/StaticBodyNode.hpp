#pragma once

#include "physics/CollisionObjectNode.hpp"

namespace saida {

// Immovable collider — level geometry, floors, walls. Never integrated.
class StaticBodyNode : public CollisionObjectNode {
public:
    StaticBodyNode() : CollisionObjectNode("StaticBody") {}

    const char* typeName() const override { return "StaticBody"; }
    BodyMotion motion() const override { return BodyMotion::Static; }

    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;
};

} // namespace saida
