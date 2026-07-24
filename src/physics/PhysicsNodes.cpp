// Serialization for the concrete physics body nodes (kept together — each is tiny).
#include "physics/StaticBodyNode.hpp"
#include "physics/RigidBodyNode.hpp"
#include "physics/AreaNode.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace saida {

void StaticBodyNode::serialize(nlohmann::json& j, ResourceManager& resources) const {
    Node::serialize(j, resources);
    serializeCommon(j);
}
void StaticBodyNode::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    Node::deserialize(j, resources);
    deserializeCommon(j);
}

void RigidBodyNode::serialize(nlohmann::json& j, ResourceManager& resources) const {
    Node::serialize(j, resources);
    serializeCommon(j);
    j["kinematic"] = kinematic;
    j["mass"] = mass;
    j["gravityFactor"] = gravityFactor;
    j["linearDamping"] = linearDamping;
    j["angularDamping"] = angularDamping;
}
void RigidBodyNode::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    Node::deserialize(j, resources);
    deserializeCommon(j);
    if (j.contains("kinematic")) kinematic = j["kinematic"].get<bool>();
    if (j.contains("mass")) mass = j["mass"].get<float>();
    if (j.contains("gravityFactor")) gravityFactor = j["gravityFactor"].get<float>();
    if (j.contains("linearDamping")) linearDamping = j["linearDamping"].get<float>();
    if (j.contains("angularDamping")) angularDamping = j["angularDamping"].get<float>();
}

void AreaNode::serialize(nlohmann::json& j, ResourceManager& resources) const {
    Node::serialize(j, resources);
    serializeCommon(j);
    j["moving"] = moving;
}
void AreaNode::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    Node::deserialize(j, resources);
    deserializeCommon(j);
    if (j.contains("moving")) moving = j["moving"].get<bool>();
}

void AreaNode::handleOverlap(CollisionObjectNode* other, bool entered) {
    if (!other) return;
    auto it = std::find(overlapping_.begin(), overlapping_.end(), other);
    if (entered) {
        if (it == overlapping_.end()) {
            overlapping_.push_back(other);
            bodyEntered.emit(other);
            bodyEnteredNamed.emit(other->name());
        }
    } else if (it != overlapping_.end()) {
        overlapping_.erase(it);
        bodyExited.emit(other);
        bodyExitedNamed.emit(other->name());
    }
}

void AreaNode::describe(reflect::TypeBuilder<AreaNode>& t) {
    t.doc("Trigger volume: fires when a physics body enters or leaves. "
          "The signal payload is the other node's name.");
    t.signal("bodyEntered", &AreaNode::bodyEnteredNamed);
    t.signal("bodyExited", &AreaNode::bodyExitedNamed);
}

} // namespace saida
