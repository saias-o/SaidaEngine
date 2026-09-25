#include "physics/CharacterBodyNode.hpp"

#include "physics/JoltGlue.hpp"

#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

#include <nlohmann/json.hpp>

namespace saida {

CharacterBodyNode::CharacterBodyNode() : CollisionObjectNode("CharacterBody") {}

CharacterBodyNode::~CharacterBodyNode() {
    character_ = nullptr;  // Ref releases the CharacterVirtual (out-of-line: complete type here)
}

JPH::BodyID CharacterBodyNode::innerBodyId() const {
    // Invalid until syncToPhysics has built the character, which is the honest
    // answer: before that the node owns no body for a query to skip.
    return character_ ? character_->GetInnerBodyID() : JPH::BodyID();
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
                                       mass, glm::radians(maxSlopeAngle),
                                       static_cast<CollisionObjectNode*>(this));
    world_ = &world;
}

void CharacterBodyNode::prePhysicsStep(PhysicsWorld& world, float dt) {
    if (!character_) return;
    if (moved_) {  // moveAndSlide already moved it this frame
        moved_ = false;
        return;
    }
    character_->SetLinearVelocity(toJolt(velocity));
    world.updateCharacter(*character_, dt);
    velocity = toGlm(character_->GetLinearVelocity());  // reflect the slide result
}

void CharacterBodyNode::syncFromPhysics(PhysicsWorld& /*world*/) {
    if (!character_) return;
    JPH::RVec3 p = character_->GetPosition();
    writeWorldPoseToLocal(glm::vec3(p.GetX(), p.GetY(), p.GetZ()), toGlm(character_->GetRotation()));
}

void CharacterBodyNode::rebasePhysics(const glm::vec3& translation, const glm::quat& rotation) {
    velocity = rotation * velocity;
    if (!character_) return;
    const auto p = character_->GetPosition();
    const glm::vec3 moved = rotation * glm::vec3(p.GetX(), p.GetY(), p.GetZ()) + translation;
    character_->SetPosition(JPH::RVec3(moved.x, moved.y, moved.z));
    character_->SetRotation(toJolt(rotation) * character_->GetRotation());
    character_->SetLinearVelocity(toJolt(velocity));
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
glm::vec3 CharacterBodyNode::moveAndSlide(const glm::vec3& v, float dt) {
    glm::vec3 position;
    glm::quat rotation;
    glm::mat4 invTR;
    // From the local transform as it is now: gameplay may have just placed
    // the node, after this frame's transforms were composed.
    const glm::mat4 world = (parent() ? parent()->worldTransform() : glm::mat4(1.0f)) * localMatrix();
    decomposeTR(world, position, rotation, invTR);
    if (!character_ || !world_ || dt <= 0.0f) {
        const glm::vec3 free = position + v * std::max(dt, 0.0f);
        writeWorldPoseToLocal(free, rotation);
        return free;
    }
    character_->SetPosition(JPH::RVec3(position.x, position.y, position.z));
    character_->SetLinearVelocity(toJolt(v));
    world_->updateCharacter(*character_, dt);
    moved_ = true;
    const JPH::RVec3 p = character_->GetPosition();
    const glm::vec3 end(float(p.GetX()), float(p.GetY()), float(p.GetZ()));
    writeWorldPoseToLocal(end, rotation);
    return end;
}

std::vector<CharacterBodyNode::Contact> CharacterBodyNode::contacts() const {
    std::vector<Contact> out;
    if (!character_ || !world_) return out;
    for (const auto& c : world_->characterContacts(*character_))
        out.push_back({static_cast<CollisionObjectNode*>(c.userData), c.point, c.normal});
    return out;
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
