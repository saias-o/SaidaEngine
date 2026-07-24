#pragma once

#include "physics/CollisionObjectNode.hpp"
#include "core/Reflection.hpp"
#include "core/Signal.hpp"

#include <string>
#include <vector>

namespace saida {

// A trigger volume (Godot Area3D): a sensor body that detects overlaps without
// generating collision response. Static by default (a fixed trigger zone).
class AreaNode : public CollisionObjectNode {
public:
    AreaNode() : CollisionObjectNode("Area") {}

    const char* typeName() const override { return "Area"; }
    BodyMotion motion() const override { return moving ? BodyMotion::Kinematic : BodyMotion::Static; }
    bool isSensor() const override { return true; }

    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;

    // Set true for a trigger that is moved around by code (e.g. attached to a character).
    bool moving = false;

    // Bodies currently inside the trigger (updated by the Scene after each step).
    const std::vector<CollisionObjectNode*>& overlapping() const { return overlapping_; }

    // Fired on the main thread when a body enters/leaves the trigger. Listen with
    // `behaviour->listen(area->bodyEntered, [](CollisionObjectNode* who){ ... })`.
    Signal<CollisionObjectNode*> bodyEntered;
    Signal<CollisionObjectNode*> bodyExited;

    // Same events with the other node's name — marshalable payload, exposed to
    // the reflection registry as "bodyEntered"/"bodyExited" so JS scripts can
    // `node.on("bodyEntered", (name) => ...)` on their Area node.
    Signal<std::string> bodyEnteredNamed;
    Signal<std::string> bodyExitedNamed;

    // Reflection: signals only — serialization stays the hand-written one above
    // (SAIDA_REFLECT_NODE would replace it and change the on-disk format).
    static constexpr const char* reflectName() { return "Area"; }
    static void describe(reflect::TypeBuilder<AreaNode>& t);

    // Called by the Scene's trigger dispatch; keeps `overlapping_` in sync.
    void handleOverlap(CollisionObjectNode* other, bool entered);

private:
    std::vector<CollisionObjectNode*> overlapping_;
};

} // namespace saida
