#include "physics/CharacterBodyNode.hpp"

#include "physics/JoltGlue.hpp"

#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <glm/gtc/matrix_transform.hpp>

#include <nlohmann/json.hpp>

namespace saida {

CharacterBodyNode::CharacterBodyNode() : CollisionObjectNode("CharacterBody") {}

CharacterBodyNode::~CharacterBodyNode() {
    character_ = nullptr;  // Ref releases the CharacterVirtual (out-of-line: complete type here)
}

void CharacterBodyNode::syncToPhysics(PhysicsWorld& world) {
    if (dirty_) {
        character_ = nullptr;  // shape/params changed → rebuild
        dirty_ = false;
    }

    glm::vec3 position;
    glm::quat rotation;
    glm::mat4 invTR;
    decomposeTR(worldTransform(), position, rotation, invTR);

    if (character_) {
        // The character owns its transform; but if a script teleported the node
        // (its world position diverged from the character's), follow the teleport.
        JPH::RVec3 cp = character_->GetPosition();
        glm::vec3 d = position - glm::vec3(cp.GetX(), cp.GetY(), cp.GetZ());
        if (glm::dot(d, d) > 1e-4f)  // > 1 cm
            character_->SetPosition(JPH::RVec3(position.x, position.y, position.z));
        // Follow the node's gameplay rotation (e.g. a controller turning the
        // character to face its movement). The capsule is rotationally symmetric,
        // so this never affects the simulation — it just keeps the authored facing
        // instead of syncFromPhysics overwriting it with a frozen orientation.
        character_->SetRotation(toJolt(rotation));
        return;
    }

    JPH::Ref<JPH::Shape> shape = buildCompoundShape(invTR);
    if (!shape) return;  // no CollisionShape child yet — try again next frame

    character_ = world.createCharacter(shape.GetPtr(), position, rotation,
                                       mass, glm::radians(maxSlopeAngle), this);
    world_ = &world;
}

void CharacterBodyNode::prePhysicsStep(PhysicsWorld& world, float dt) {
    if (!character_) return;
    character_->SetLinearVelocity(toJolt(velocity));
    world.updateCharacter(*character_, dt);
    velocity = toGlm(character_->GetLinearVelocity());  // reflect the slide result
}

void CharacterBodyNode::syncFromPhysics(PhysicsWorld& /*world*/) {
    if (!character_) return;
    JPH::RVec3 p = character_->GetPosition();
    writeWorldPoseToLocal(glm::vec3(p.GetX(), p.GetY(), p.GetZ()), toGlm(character_->GetRotation()));
}

bool CharacterBodyNode::isOnFloor() const {
    return character_ && character_->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}
bool CharacterBodyNode::isOnSteepSlope() const {
    return character_ &&
           character_->GetGroundState() == JPH::CharacterBase::EGroundState::OnSteepGround;
}
glm::vec3 CharacterBodyNode::groundNormal() const {
    return character_ ? toGlm(character_->GetGroundNormal()) : glm::vec3(0.0f, 1.0f, 0.0f);
}
glm::vec3 CharacterBodyNode::groundVelocity() const {
    return character_ ? toGlm(character_->GetGroundVelocity()) : glm::vec3(0.0f);
}

void CharacterBodyNode::serialize(nlohmann::json& j, ResourceManager& resources) const {
    Node::serialize(j, resources);
    serializeCommon(j);
    j["mass"] = mass;
    j["maxSlopeAngle"] = maxSlopeAngle;
}
void CharacterBodyNode::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    Node::deserialize(j, resources);
    deserializeCommon(j);
    if (j.contains("mass")) mass = j["mass"].get<float>();
    if (j.contains("maxSlopeAngle")) maxSlopeAngle = j["maxSlopeAngle"].get<float>();
}

} // namespace saida
