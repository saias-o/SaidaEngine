#include "scene/animation/ProceduralNodes.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace saida {

namespace {

constexpr float kEpsilon = 1e-5f;

glm::vec3 positionOf(const glm::mat4& m) { return glm::vec3(m[3]); }

glm::quat rotationOf(const glm::mat4& m) { return glm::quat_cast(glm::mat3(m)); }

// Minimal rotation mapping `from` onto `to` (arbitrary non-zero vectors).
glm::quat rotationBetween(const glm::vec3& from, const glm::vec3& to) {
    const float fromLength = glm::length(from);
    const float toLength = glm::length(to);
    if (fromLength < kEpsilon || toLength < kEpsilon) return glm::quat(1, 0, 0, 0);

    const glm::vec3 f = from / fromLength;
    const glm::vec3 t = to / toLength;
    const float cosine = glm::dot(f, t);
    if (cosine > 1.0f - kEpsilon) return glm::quat(1, 0, 0, 0);
    if (cosine < -1.0f + kEpsilon) {
        // 180-degree turn: any perpendicular axis works.
        glm::vec3 axis = glm::cross(f, glm::vec3(1, 0, 0));
        if (glm::dot(axis, axis) < kEpsilon) axis = glm::cross(f, glm::vec3(0, 1, 0));
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    return glm::angleAxis(std::acos(std::clamp(cosine, -1.0f, 1.0f)),
                          glm::normalize(glm::cross(f, t)));
}

glm::quat parentGlobalRotation(const GlobalPose& global, const Rig& rig, int32_t bone) {
    const int32_t parent = rig.bones()[size_t(bone)].parentIndex;
    return parent >= 0 ? rotationOf(global.globalMatrices[size_t(parent)])
                       : glm::quat(1, 0, 0, 0);
}

void evaluateInput(const AnimNode* input, const LocalPose& bindPose, LocalPose& outPose) {
    if (input) {
        input->evaluate(bindPose, outPose);
    } else {
        outPose = bindPose;
    }
}

void blendLocalRotation(Transform& transform, const glm::quat& solved, float weight) {
    transform.rotation = weight >= 1.0f
                             ? solved
                             : glm::slerp(transform.rotation, solved, std::max(weight, 0.0f));
}

} // namespace

TwoBoneIKNode::TwoBoneIKNode(const Rig& rig, const std::string& rootBone,
                             const std::string& midBone, const std::string& tipBone)
    : rig_(&rig),
      rootIndex_(rig.findBoneIndex(rootBone)),
      midIndex_(rig.findBoneIndex(midBone)),
      tipIndex_(rig.findBoneIndex(tipBone)) {}

void TwoBoneIKNode::setPole(const glm::vec3& pole) {
    pole_ = pole;
    hasPole_ = true;
}

void TwoBoneIKNode::update(float deltaTime) {
    if (input_) input_->update(deltaTime);
}

void TwoBoneIKNode::evaluate(const LocalPose& bindPose, LocalPose& outPose) const {
    evaluateInput(input_.get(), bindPose, outPose);
    if (!valid() || weight_ <= 0.0f) return;

    scratchGlobal_.computeFrom(outPose, *rig_);
    const glm::vec3 rootPos = positionOf(scratchGlobal_.globalMatrices[size_t(rootIndex_)]);
    const glm::vec3 midPos = positionOf(scratchGlobal_.globalMatrices[size_t(midIndex_)]);
    const glm::vec3 tipPos = positionOf(scratchGlobal_.globalMatrices[size_t(tipIndex_)]);

    const float upperLength = glm::length(midPos - rootPos);
    const float lowerLength = glm::length(tipPos - midPos);
    if (upperLength < kEpsilon || lowerLength < kEpsilon) return;

    const glm::vec3 toTarget = target_ - rootPos;
    const float reach = std::clamp(glm::length(toTarget), kEpsilon,
                                   upperLength + lowerLength - kEpsilon);

    // 1) Bend: the interior elbow angle achieves the root->target distance.
    const glm::vec3 upper = midPos - rootPos;
    const glm::vec3 lower = tipPos - midPos;
    const float currentCos = glm::dot(glm::normalize(rootPos - midPos),
                                      glm::normalize(tipPos - midPos));
    const float desiredCos = std::clamp(
        (upperLength * upperLength + lowerLength * lowerLength - reach * reach) /
            (2.0f * upperLength * lowerLength),
        -1.0f, 1.0f);
    glm::vec3 bendAxis = glm::cross(upper, lower);
    if (glm::dot(bendAxis, bendAxis) < kEpsilon) {
        // Perfectly straight chain: bend toward the pole, otherwise toward
        // an arbitrary perpendicular axis.
        const glm::vec3 hint = hasPole_ ? pole_ - rootPos : glm::vec3(0, 0, 1);
        bendAxis = glm::cross(upper, hint);
        if (glm::dot(bendAxis, bendAxis) < kEpsilon)
            bendAxis = glm::cross(upper, glm::vec3(1, 0, 0));
    }
    bendAxis = glm::normalize(bendAxis);

    // The angle between segments (pi - interior) grows by rotating `lower`
    // around upper x lower; we go from the current angle to the desired one.
    const float currentInterior = std::acos(std::clamp(currentCos, -1.0f, 1.0f));
    const float desiredInterior = std::acos(desiredCos);
    const glm::quat bend = glm::angleAxis(currentInterior - desiredInterior, bendAxis);

    glm::quat midGlobal = bend * rotationOf(scratchGlobal_.globalMatrices[size_t(midIndex_)]);
    const glm::vec3 bentTip = midPos + bend * lower;

    // 2) Aim: the root brings the bent tip onto the target.
    glm::quat aim = rotationBetween(bentTip - rootPos, toTarget);

    // 3) Pole: roll around the root->target axis to orient the elbow.
    // Signed angle around the known axis — rotationBetween would pick an
    // arbitrary axis when the elbow is opposite the pole.
    if (hasPole_) {
        const glm::vec3 axis = glm::normalize(toTarget);
        const glm::vec3 midOffset = aim * upper;
        const glm::vec3 poleOffset = pole_ - rootPos;
        const glm::vec3 midProjected = midOffset - axis * glm::dot(midOffset, axis);
        const glm::vec3 poleProjected = poleOffset - axis * glm::dot(poleOffset, axis);
        if (glm::dot(midProjected, midProjected) > kEpsilon &&
            glm::dot(poleProjected, poleProjected) > kEpsilon) {
            const glm::vec3 m = glm::normalize(midProjected);
            const glm::vec3 p = glm::normalize(poleProjected);
            const float roll = std::atan2(glm::dot(axis, glm::cross(m, p)), glm::dot(m, p));
            aim = glm::angleAxis(roll, axis) * aim;
        }
    }

    const glm::quat rootGlobal =
        aim * rotationOf(scratchGlobal_.globalMatrices[size_t(rootIndex_)]);
    midGlobal = aim * midGlobal;

    // Back to local space (the elbow's parent is the chain's root).
    const glm::quat rootParent = parentGlobalRotation(scratchGlobal_, *rig_, rootIndex_);
    blendLocalRotation(outPose.localTransforms[size_t(rootIndex_)],
                       glm::inverse(rootParent) * rootGlobal, weight_);
    blendLocalRotation(outPose.localTransforms[size_t(midIndex_)],
                       glm::inverse(rootGlobal) * midGlobal, weight_);
}

LookAtNode::LookAtNode(const Rig& rig, const std::string& bone, const glm::vec3& forwardAxis)
    : rig_(&rig), boneIndex_(rig.findBoneIndex(bone)), forwardAxis_(forwardAxis) {}

void LookAtNode::update(float deltaTime) {
    if (input_) input_->update(deltaTime);
}

void LookAtNode::evaluate(const LocalPose& bindPose, LocalPose& outPose) const {
    evaluateInput(input_.get(), bindPose, outPose);
    if (!valid() || weight_ <= 0.0f) return;

    scratchGlobal_.computeFrom(outPose, *rig_);
    const glm::vec3 bonePos = positionOf(scratchGlobal_.globalMatrices[size_t(boneIndex_)]);
    const glm::quat boneGlobal = rotationOf(scratchGlobal_.globalMatrices[size_t(boneIndex_)]);

    const glm::vec3 toTarget = target_ - bonePos;
    if (glm::dot(toTarget, toTarget) < kEpsilon) return;

    const glm::quat correction = rotationBetween(boneGlobal * forwardAxis_, toTarget);
    const glm::quat parent = parentGlobalRotation(scratchGlobal_, *rig_, boneIndex_);
    blendLocalRotation(outPose.localTransforms[size_t(boneIndex_)],
                       glm::inverse(parent) * (correction * boneGlobal), weight_);
}

} // namespace saida
