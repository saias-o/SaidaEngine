#include "physics/Ragdoll.hpp"

#include "physics/JoltGlue.hpp"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace saida {

using namespace JPH;

namespace {

constexpr float kEpsilon = 1e-6f;
// Shortest capsule kept: Jolt asserts on a zero cylinder.
constexpr float kMinimumHalfCylinder = 0.00001f;
// Group ids handed out to ragdolls so that two of them collide with each other
// while each keeps its own parts apart. cInvalidGroup is never used.
std::atomic<uint32_t> g_nextGroup{1};

glm::vec3 positionOf(const glm::mat4& m) { return glm::vec3(m[3]); }

// A rotation out of a matrix whatever (uniform or not) scale it carries.
glm::quat rotationOf(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int c = 0; c < 3; ++c) {
        const float length = glm::length(r[c]);
        if (length > kEpsilon) r[c] /= length;
    }
    return glm::normalize(glm::quat_cast(r));
}

glm::quat rotationBetween(const glm::vec3& from, const glm::vec3& to) {
    const glm::vec3 f = glm::normalize(from), t = glm::normalize(to);
    const float cosine = glm::dot(f, t);
    if (cosine > 1.0f - 1e-6f) return glm::quat(1, 0, 0, 0);
    if (cosine < -1.0f + 1e-6f) {
        glm::vec3 axis = glm::cross(f, glm::vec3(1, 0, 0));
        if (glm::dot(axis, axis) < 1e-6f) axis = glm::cross(f, glm::vec3(0, 1, 0));
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    return glm::angleAxis(std::acos(std::clamp(cosine, -1.0f, 1.0f)), glm::normalize(glm::cross(f, t)));
}

// A unit vector perpendicular to `axis`, as close to `hint` as can be.
glm::vec3 perpendicular(const glm::vec3& axis, const glm::vec3& hint) {
    glm::vec3 p = hint - axis * glm::dot(hint, axis);
    if (glm::dot(p, p) < 1e-6f) {
        const glm::vec3 other = std::abs(axis.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 0, 1);
        p = other - axis * glm::dot(other, axis);
    }
    return glm::normalize(p);
}

float uniformScale(const glm::mat4& m) { return glm::length(glm::vec3(m[0])); }

} // namespace

RagdollProfile humanoidRagdoll(const HumanoidBones& b, float mass, const glm::vec3& forward, float limbRadius) {
    RagdollProfile profile;
    profile.mass = mass;
    const glm::vec3 front = glm::normalize(forward);
    auto add = [&](RagdollPart part) { profile.parts.push_back(std::move(part)); };
    RagdollPart pelvis;
    pelvis.bone = b.pelvis; pelvis.end = b.spine.empty() ? b.chest : b.spine;
    pelvis.radius = limbRadius * 2.2f; pelvis.massShare = 0.18f;
    add(pelvis);
    RagdollPart chest;
    chest.bone = b.chest; chest.end = b.neck.empty() ? b.head : b.neck;
    chest.radius = limbRadius * 2.3f; chest.massShare = 0.22f; chest.swing = 0.45f; chest.twist = 0.35f;
    chest.bendToward = front; chest.swingTilt = 0.15f;
    add(chest);
    RagdollPart head;
    head.bone = b.head; head.length = limbRadius * 3.2f; head.axis = glm::vec3(0, 1, 0);
    head.radius = limbRadius * 1.6f; head.massShare = 0.08f; head.swing = 0.6f; head.twist = 0.7f;
    head.bendToward = front; head.swingTilt = 0.1f;
    add(head);
    for (int side = 0; side < 2; ++side) {
        RagdollPart upper;
        upper.bone = side ? b.rightUpperArm : b.leftUpperArm; upper.end = side ? b.rightForearm : b.leftForearm;
        // From a T-pose the arm mostly hangs and swings below the shoulder.
        upper.radius = limbRadius * 0.9f; upper.massShare = 0.035f; upper.swing = 1.25f; upper.twist = 0.7f;
        upper.bendToward = glm::vec3(0, -1, 0); upper.swingTilt = 0.85f;
        add(upper);
        RagdollPart fore;
        fore.bone = side ? b.rightForearm : b.leftForearm; fore.end = side ? b.rightHand : b.leftHand;
        fore.radius = limbRadius * 0.75f; fore.massShare = 0.025f;
        fore.joint = RagdollJoint::Hinge; fore.bendToward = front; fore.bendMin = -0.05f; fore.bendMax = 2.5f;
        add(fore);
    }
    for (int side = 0; side < 2; ++side) {
        RagdollPart thigh;
        thigh.bone = side ? b.rightThigh : b.leftThigh; thigh.end = side ? b.rightShin : b.leftShin;
        // A hip folds forward about 90 degrees and extends about 25.
        thigh.radius = limbRadius * 1.25f; thigh.massShare = 0.11f; thigh.swing = 1.0f; thigh.twist = 0.4f;
        thigh.bendToward = front; thigh.swingTilt = 0.55f;
        add(thigh);
        RagdollPart shin;
        shin.bone = side ? b.rightShin : b.leftShin; shin.end = side ? b.rightFoot : b.leftFoot;
        shin.radius = limbRadius * 0.9f; shin.massShare = 0.055f;
        shin.joint = RagdollJoint::Hinge; shin.bendToward = -front; shin.bendMin = -0.05f; shin.bendMax = 2.4f;
        add(shin);
    }
    return profile;
}

Ragdoll::Ragdoll(const Rig& rig, RagdollProfile profile) : rig_(rig), profile_(std::move(profile)) { build(rig); }

Ragdoll::~Ragdoll() { despawn(); }

void Ragdoll::build(const Rig& rig) {
    const size_t bones = rig.boneCount();
    if (!rig.finalized() || bones == 0) { error_ = "the rig is not finalized"; return; }
    if (profile_.parts.empty()) { error_ = "the profile has no part"; return; }
    restGlobal_.resize(bones);
    for (size_t i = 0; i < bones; ++i) restGlobal_[i] = glm::inverse(rig.bones()[i].inverseBindMatrix);
    partOfBone_.assign(bones, -1);
    float shares = 0.0f;
    for (size_t i = 0; i < profile_.parts.size(); ++i) {
        const RagdollPart& def = profile_.parts[i];
        Part part;
        part.bone = rig.findBoneIndex(def.bone);
        if (part.bone < 0) { error_ = "no bone '" + def.bone + "'"; return; }
        if (partOfBone_[size_t(part.bone)] >= 0) { error_ = "bone '" + def.bone + "' drives two parts"; return; }
        if (!def.end.empty()) {
            part.end = rig.findBoneIndex(def.end);
            if (part.end < 0) { error_ = "no bone '" + def.end + "'"; return; }
        }
        for (int32_t a = rig.bones()[size_t(part.bone)].parentIndex; a >= 0; a = rig.bones()[size_t(a)].parentIndex)
            if (partOfBone_[size_t(a)] >= 0) { part.parent = partOfBone_[size_t(a)]; break; }
        part.drivesPosition = i == 0 || part.parent < 0;
        if (!def.parent.empty()) {
            const int32_t parentBone = rig.findBoneIndex(def.parent);
            if (parentBone < 0 || partOfBone_[size_t(parentBone)] < 0) {
                error_ = "part '" + def.bone + "' names no earlier parent part '" + def.parent + "'"; return;
            }
            part.drivesPosition = part.drivesPosition || part.parent != partOfBone_[size_t(parentBone)];
            part.parent = partOfBone_[size_t(parentBone)];
        }
        if (i > 0 && part.parent < 0) { error_ = "part '" + def.bone + "' has no part among its ancestors"; return; }
        const glm::vec3 origin = positionOf(restGlobal_[size_t(part.bone)]);
        const glm::vec3 reach = part.end >= 0 ? positionOf(restGlobal_[size_t(part.end)]) - origin
                                              : glm::normalize(def.axis) * def.length;
        part.length = glm::length(reach);
        if (!(part.length > 1e-4f)) { error_ = "part '" + def.bone + "' has no length"; return; }
        part.restDirection = glm::normalize(glm::inverse(rotationOf(restGlobal_[size_t(part.bone)])) * (reach / part.length));
        part.radius = std::max(def.radius, 1e-3f);
        shares += std::max(def.massShare, 0.0f);
        partOfBone_[size_t(part.bone)] = int32_t(i);
        parts_.push_back(part);
    }
    if (!(shares > 0.0f)) { error_ = "the parts have no mass"; return; }
    for (size_t i = 0; i < parts_.size(); ++i)
        parts_[i].mass = std::max(profile_.parts[i].massShare, 0.0f) / shares * profile_.mass;
    // A part never collides with the part it hangs from, nor with a sibling
    // sharing that joint: they overlap there by construction.
    filter_ = new GroupFilterTable(uint(parts_.size()));
    for (size_t i = 0; i < parts_.size(); ++i)
        for (size_t j = i + 1; j < parts_.size(); ++j)
            if (parts_[j].parent == int32_t(i) || parts_[i].parent == int32_t(j) ||
                (parts_[i].parent >= 0 && parts_[i].parent == parts_[j].parent))
                filter_->DisableCollision(CollisionGroup::SubGroupID(i), CollisionGroup::SubGroupID(j));
}

bool Ragdoll::spawn(PhysicsWorld& world, const GlobalPose& pose, const LocalPose& local,
                    const glm::mat4& objectToWorld, const glm::vec3& velocity, const glm::vec3& angularVelocity) {
    const size_t bones = rig_.boneCount();
    if (!valid() || world_ || pose.globalMatrices.size() < bones || local.localTransforms.size() < bones)
        return false;
    const float scale = uniformScale(objectToWorld);
    if (!(scale > kEpsilon)) return false;
    const glm::quat toWorld = rotationOf(objectToWorld);
    captured_ = local;
    worldScale_ = scale;
    uint32_t group = g_nextGroup.fetch_add(1);
    if (group == CollisionGroup::cInvalidGroup) group = g_nextGroup.fetch_add(1);
    group_ = group;

    std::vector<glm::vec3> pivots(parts_.size()), centres(parts_.size());
    std::vector<glm::quat> bodyRotations(parts_.size());
    auto abandon = [&] {
        for (Part& part : parts_) {
            if (!part.body.IsInvalid()) world.removeBody(part.body);
            part.body = BodyID();
        }
        return false;
    };
    const glm::vec3 rootPivot = glm::vec3(objectToWorld * glm::vec4(positionOf(pose.globalMatrices[size_t(parts_[0].bone)]), 1.0f));
    for (size_t i = 0; i < parts_.size(); ++i) {
        Part& part = parts_[i];
        const glm::mat4& global = pose.globalMatrices[size_t(part.bone)];
        const glm::vec3 pivot = glm::vec3(objectToWorld * glm::vec4(positionOf(global), 1.0f));
        const glm::quat boneRotation = glm::normalize(toWorld * rotationOf(global));
        const glm::vec3 posedAxis = boneRotation * part.restDirection;
        glm::vec3 reach = posedAxis * (part.length * scale);
        if (part.end >= 0) {
            const glm::vec3 end = glm::vec3(objectToWorld * glm::vec4(positionOf(pose.globalMatrices[size_t(part.end)]), 1.0f));
            if (glm::length(end - pivot) > 1e-4f) reach = end - pivot;
        }
        const float length = glm::length(reach);
        const glm::vec3 axis = reach / length;
        const float radius = part.radius * scale;
        // The capsule's tips reach the joint at either end; the parts overlap
        // there, which the group filter allows.
        const float halfCylinder = std::max(kMinimumHalfCylinder, 0.5f * length - radius);
        const glm::quat bodyRotation = glm::normalize(rotationBetween(posedAxis, axis) * boneRotation *
                                                      rotationBetween(glm::vec3(0, 1, 0), part.restDirection));
        const glm::vec3 centre = pivot + axis * (0.5f * length);
        Ref<Shape> shape = new CapsuleShape(halfCylinder, radius);
        BodyDesc desc;
        desc.shape = shape.GetPtr();
        desc.position = centre;
        desc.rotation = bodyRotation;
        desc.motion = BodyMotion::Dynamic;
        desc.friction = profile_.friction;
        desc.restitution = profile_.restitution;
        desc.mass = part.mass;
        desc.linearDamping = profile_.linearDamping;
        desc.angularDamping = profile_.angularDamping;
        // A body struck by a car crosses a wall's thickness in one step.
        desc.continuousCollision = true;
        desc.groupFilter = filter_.GetPtr();
        desc.collisionGroup = group_;
        desc.collisionSubGroup = uint32_t(i);
        part.body = world.createBody(desc);
        if (part.body.IsInvalid()) return abandon();
        world.setLinearVelocity(part.body, velocity + glm::cross(angularVelocity, centre - rootPivot));
        if (glm::dot(angularVelocity, angularVelocity) > 0.0f) world.setAngularVelocity(part.body, angularVelocity);
        const glm::quat inverseBody = glm::inverse(bodyRotation);
        part.boneInBodyRotation = glm::normalize(inverseBody * boneRotation);
        part.boneInBodyPosition = inverseBody * (pivot - centre);
        pivots[i] = pivot;
        centres[i] = centre;
        bodyRotations[i] = bodyRotation;
    }

    // Joints, in each body's own frame, with the neutral angle taken from the
    // bind pose: the child's frame as it sat in its parent's at rest.
    const BodyLockInterfaceNoLock& locks = world.system().GetBodyLockInterfaceNoLock();
    for (size_t i = 1; i < parts_.size(); ++i) {
        const Part& child = parts_[i];
        const Part& parent = parts_[size_t(child.parent)];
        Body* body1 = locks.TryGetBody(parent.body);
        Body* body2 = locks.TryGetBody(child.body);
        if (!body1 || !body2) return abandon();
        const RagdollPart& def = profile_.parts[i];
        const glm::quat restParent = rotationOf(restGlobal_[size_t(parent.bone)]);
        const glm::quat restChild = rotationOf(restGlobal_[size_t(child.bone)]);
        const glm::quat childInParent = glm::normalize(glm::inverse(restParent) * restChild);
        const glm::vec3 restAxis = restChild * child.restDirection;  // object space, bind pose
        // Child-bone-local vectors, then in each body's frame.
        auto inChild = [&](const glm::vec3& v) { return glm::normalize(child.boneInBodyRotation * v); };
        auto inParent = [&](const glm::vec3& v) { return glm::normalize(parent.boneInBodyRotation * (childInParent * v)); };
        const glm::vec3 pivot1 = glm::inverse(bodyRotations[size_t(child.parent)]) * (pivots[i] - centres[size_t(child.parent)]);
        const glm::vec3 pivot2 = glm::inverse(bodyRotations[i]) * (pivots[i] - centres[i]);
        TwoBodyConstraint* constraint = nullptr;
        if (def.joint == RagdollJoint::Hinge) {
            const glm::vec3 hingeObject = perpendicular(restAxis, glm::cross(restAxis, glm::normalize(def.bendToward)));
            const glm::vec3 hinge = glm::inverse(restChild) * hingeObject;
            const glm::vec3 normal = child.restDirection;
            HingeConstraintSettings settings;
            settings.mSpace = EConstraintSpace::LocalToBodyCOM;
            settings.mPoint1 = RVec3(pivot1.x, pivot1.y, pivot1.z);
            settings.mPoint2 = RVec3(pivot2.x, pivot2.y, pivot2.z);
            settings.mHingeAxis1 = toJolt(inParent(hinge));
            settings.mNormalAxis1 = toJolt(inParent(normal));
            settings.mHingeAxis2 = toJolt(inChild(hinge));
            settings.mNormalAxis2 = toJolt(inChild(normal));
            settings.mLimitsMin = std::min(def.bendMin, def.bendMax);
            settings.mLimitsMax = std::max(def.bendMin, def.bendMax);
            settings.mMaxFrictionTorque = profile_.jointFriction;
            constraint = settings.Create(*body1, *body2);
        } else {
            // The plane axis faces the way the body faces at rest where it can,
            // so a cone's two half angles mean forward/back and left/right.
            const glm::vec3 planeObject = perpendicular(restAxis, glm::vec3(0, 0, 1));
            const glm::vec3 twist = child.restDirection;
            const glm::vec3 plane = glm::inverse(restChild) * planeObject;
            // The parent holds the cone's centre leaning toward bendToward.
            glm::quat lean(1.0f, 0.0f, 0.0f, 0.0f);
            const glm::vec3 leanAxis = glm::cross(restAxis, glm::normalize(def.bendToward));
            if (def.swingTilt != 0.0f && glm::dot(leanAxis, leanAxis) > 1e-6f)
                lean = glm::angleAxis(def.swingTilt, glm::normalize(leanAxis));
            const glm::vec3 centreTwist = glm::inverse(restChild) * (lean * restAxis);
            const glm::vec3 centrePlane = glm::inverse(restChild) * (lean * planeObject);
            SwingTwistConstraintSettings settings;
            settings.mSpace = EConstraintSpace::LocalToBodyCOM;
            settings.mPosition1 = RVec3(pivot1.x, pivot1.y, pivot1.z);
            settings.mPosition2 = RVec3(pivot2.x, pivot2.y, pivot2.z);
            settings.mTwistAxis1 = toJolt(inParent(centreTwist));
            settings.mPlaneAxis1 = toJolt(inParent(centrePlane));
            settings.mTwistAxis2 = toJolt(inChild(twist));
            settings.mPlaneAxis2 = toJolt(inChild(plane));
            settings.mNormalHalfConeAngle = std::max(def.swing, 0.0f);
            settings.mPlaneHalfConeAngle = std::max(def.swing, 0.0f);
            settings.mTwistMinAngle = -std::abs(def.twist);
            settings.mTwistMaxAngle = std::abs(def.twist);
            settings.mMaxFrictionTorque = profile_.jointFriction;
            constraint = settings.Create(*body1, *body2);
        }
        world.addConstraint(Ref<TwoBodyConstraint>(constraint));
    }
    world_ = &world;
    calm_ = 0.0f;
    settled_ = false;
    return true;
}

void Ragdoll::despawn() {
    if (!world_) return;
    // Removing a body drops the constraints attached to it.
    for (Part& part : parts_) {
        if (!part.body.IsInvalid()) world_->removeBody(part.body);
        part.body = BodyID();
    }
    world_ = nullptr;
    calm_ = 0.0f;
    settled_ = false;
}

void Ragdoll::readPose(const glm::mat4& objectToWorld, LocalPose& out) const {
    const size_t bones = rig_.boneCount();
    out = captured_;
    if (!world_ || out.localTransforms.size() < bones) return;
    const glm::mat4 toObject = glm::inverse(objectToWorld);
    const glm::quat toObjectRotation = glm::inverse(rotationOf(objectToWorld));
    scratch_.resize(bones);
    for (uint32_t b : rig_.evaluationOrder()) {
        const int32_t parentBone = rig_.bones()[b].parentIndex;
        const glm::mat4 above = parentBone >= 0 ? scratch_[size_t(parentBone)] : glm::mat4(1.0f);
        Transform& local = out.localTransforms[b];
        const int32_t p = partOfBone_[b];
        if (p >= 0) {
            const Part& part = parts_[size_t(p)];
            glm::vec3 bodyPosition;
            glm::quat bodyRotation;
            world_->getBodyTransform(part.body, bodyPosition, bodyRotation);
            const glm::quat rotation = glm::normalize(toObjectRotation * bodyRotation * part.boneInBodyRotation);
            const glm::quat aboveRotation = parentBone >= 0 ? rotationOf(above) : glm::quat(1, 0, 0, 0);
            local.rotation = glm::normalize(glm::inverse(aboveRotation) * rotation);
            if (part.drivesPosition) {
                const glm::vec3 position = bodyPosition + bodyRotation * part.boneInBodyPosition;
                local.position = glm::vec3(glm::inverse(above) * (toObject * glm::vec4(position, 1.0f)));
            }
        }
        scratch_[b] = above * local.matrix();
    }
}

void Ragdoll::applyImpulse(const glm::vec3& impulse, const glm::vec3& worldPoint) {
    if (world_) applyImpulse(nearestPart(worldPoint), impulse, worldPoint);
}

void Ragdoll::applyImpulse(size_t part, const glm::vec3& impulse, const glm::vec3& worldPoint) {
    if (!world_ || part >= parts_.size()) return;
    world_->applyImpulse(parts_[part].body, impulse, worldPoint);
}

int Ragdoll::partOf(JPH::BodyID body) const {
    if (!world_ || body.IsInvalid()) return -1;
    for (size_t i = 0; i < parts_.size(); ++i)
        if (parts_[i].body == body) return int(i);
    return -1;
}

size_t Ragdoll::nearestPart(const glm::vec3& worldPoint) const {
    size_t best = 0;
    float bestDistance = std::numeric_limits<float>::max();
    for (size_t i = 0; i < parts_.size(); ++i) {
        const float d = glm::length(partPosition(i) - worldPoint);
        if (d < bestDistance) { bestDistance = d; best = i; }
    }
    return best;
}

JPH::BodyID Ragdoll::body(size_t part) const { return part < parts_.size() ? parts_[part].body : BodyID(); }

glm::vec3 Ragdoll::partPosition(size_t part) const {
    if (!world_ || part >= parts_.size()) return glm::vec3(0.0f);
    glm::vec3 position;
    glm::quat rotation;
    world_->getBodyTransform(parts_[part].body, position, rotation);
    return position;
}

glm::vec3 Ragdoll::bonePosition(size_t part) const {
    if (!world_ || part >= parts_.size()) return glm::vec3(0.0f);
    glm::vec3 position;
    glm::quat rotation;
    world_->getBodyTransform(parts_[part].body, position, rotation);
    return position + rotation * parts_[part].boneInBodyPosition;
}

glm::vec3 Ragdoll::rootPosition() const { return bonePosition(0); }

glm::quat Ragdoll::rootRotation() const {
    if (!world_) return glm::quat(1, 0, 0, 0);
    glm::vec3 position;
    glm::quat rotation;
    world_->getBodyTransform(parts_[0].body, position, rotation);
    return glm::normalize(rotation * parts_[0].boneInBodyRotation);
}

float Ragdoll::fastestSpeed() const {
    float fastest = 0.0f;
    if (!world_) return fastest;
    for (const Part& part : parts_) fastest = std::max(fastest, glm::length(world_->linearVelocity(part.body)));
    return fastest;
}

bool Ragdoll::updateSettled(float dt) {
    if (!world_) return false;
    bool asleep = true;
    for (const Part& part : parts_) asleep = asleep && !world_->isBodyActive(part.body);
    if (asleep || fastestSpeed() < profile_.settleSpeed) calm_ += std::max(dt, 0.0f);
    else calm_ = 0.0f;
    settled_ = asleep || calm_ >= profile_.settleTime;
    return settled_;
}

void Ragdoll::rebase(const glm::vec3& translation, const glm::quat& rotation) {
    if (!world_) return;
    for (const Part& part : parts_) {
        glm::vec3 position;
        glm::quat orientation;
        world_->getBodyTransform(part.body, position, orientation);
        const bool awake = world_->isBodyActive(part.body);
        const glm::vec3 linear = world_->linearVelocity(part.body), angular = world_->angularVelocity(part.body);
        world_->setBodyTransform(part.body, rotation * position + translation, glm::normalize(rotation * orientation), false);
        if (awake) {
            world_->setLinearVelocity(part.body, rotation * linear);
            world_->setAngularVelocity(part.body, rotation * angular);
        }
    }
}

} // namespace saida
