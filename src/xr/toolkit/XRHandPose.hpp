#pragma once

#include "scene/animation/Pose.hpp"
#include "xr/toolkit/XRTypes.hpp"

#include <glm/gtc/quaternion.hpp>
#include <string>

namespace saida {

// The MIT Godot OpenXR demo uses humanoid bone axes: +Y along the finger,
// -Z out the back of the hand. OpenXR uses -Z along the finger, +Y dorsal.
// Matches Godot's OpenXR hand-tracking bone adjustment, in quaternion wxyz.
inline glm::quat xrHandHumanoidAxes() {
    constexpr float s = 0.7071067811865475f;
    return {0.0f, 0.0f, -s, s};
}

class XRHandPose {
public:
    XRHandPose(const Rig& rig, XRHand hand) : rig_(rig), globals_(rig.boneCount()) {
        const std::string prefix = hand == XRHand::Left ? "Left" : "Right";
        constexpr const char* names[kXRHandJointCount] = {
            "Palm", "Hand", "ThumbMetacarpal", "ThumbProximal", "ThumbDistal", "ThumbTip",
            "IndexMetacarpal", "IndexProximal", "IndexIntermediate", "IndexDistal", "IndexTip",
            "MiddleMetacarpal", "MiddleProximal", "MiddleIntermediate", "MiddleDistal", "MiddleTip",
            "RingMetacarpal", "RingProximal", "RingIntermediate", "RingDistal", "RingTip",
            "LittleMetacarpal", "LittleProximal", "LittleIntermediate", "LittleDistal", "LittleTip"
        };
        joints_.resize(rig.boneCount(), -1);
        valid_ = rig.finalized() && rig.boneCount() == kXRHandJointCount - 1;
        for (int j = 1; j < kXRHandJointCount; ++j) {
            const int bone = rig.findBoneIndex(prefix + names[j]);
            if (bone < 0) valid_ = false;
            else joints_[static_cast<size_t>(bone)] = j;
        }
    }

    bool valid() const { return valid_; }

    void evaluate(const XRHandSkeletonState& skeleton, const glm::mat4& modelToWorld,
                  LocalPose& pose) const {
        pose.resize(rig_.boneCount());
        const glm::mat4 worldToModel = glm::inverse(modelToWorld);
        const glm::mat4 axes = glm::mat4_cast(xrHandHumanoidAxes());
        for (uint32_t i : rig_.evaluationOrder()) {
            const auto& bone = rig_.bones()[i];
            const glm::mat4 parent = bone.parentIndex >= 0
                ? globals_[static_cast<size_t>(bone.parentIndex)] : glm::mat4(1.0f);
            const int jointIndex = joints_[i];
            const auto* joint = jointIndex >= 0 ? &skeleton.joints[static_cast<size_t>(jointIndex)] : nullptr;
            if (valid_ && skeleton.active && joint && joint->valid) {
                glm::mat4 tracked = glm::mat4_cast(joint->pose.orientation);
                tracked[3] = glm::vec4(joint->pose.position, 1.0f);
                globals_[i] = worldToModel * tracked * axes;
                const glm::mat4 local = glm::inverse(parent) * globals_[i];
                auto& transform = pose.localTransforms[i];
                transform.position = glm::vec3(local[3]);
                transform.scale = {glm::length(glm::vec3(local[0])),
                                   glm::length(glm::vec3(local[1])),
                                   glm::length(glm::vec3(local[2]))};
                glm::mat3 rotation(local);
                for (int c = 0; c < 3; ++c) rotation[c] /= transform.scale[c];
                transform.rotation = glm::normalize(glm::quat_cast(rotation));
            } else {
                // An unavailable joint follows its parent in the authored rest
                // pose; never treat an invalid runtime pose as an origin pose.
                pose.localTransforms[i] = bone.restLocal;
                globals_[i] = parent * bone.restLocal.matrix();
            }
        }
    }

private:
    const Rig& rig_;
    std::vector<int> joints_;
    mutable std::vector<glm::mat4> globals_;
    bool valid_ = false;
};

} // namespace saida
