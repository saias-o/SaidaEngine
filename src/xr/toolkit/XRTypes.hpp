#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>

// Shared XR toolkit types. Toolkit code reads XRInput, not OpenXR directly.

namespace saida {

// Which hand a controller / interactor represents.
enum class XRHand { Left = 0, Right = 1 };
inline constexpr int kXRHandCount = 2;
inline int xrHandIndex(XRHand h) { return static_cast<int>(h); }

// A tracked 6-DoF pose. `tracked` is false when the runtime lost tracking this
// frame (consumers should hold their last good pose rather than snap to origin).
struct XRPose {
    bool tracked = false;
    glm::vec3 position{0.0f};
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};

    glm::mat4 matrix() const {
        return glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(orientation);
    }
};

// One controller/hand sample. Buttons are raw analog values; XRInput derives edges.
struct XRHandState {
    bool active = false;          // device present & tracked this frame
    XRPose grip;                  // grip pose — for holding objects
    XRPose aim;                   // aim/pointer pose — for ray interactors
    float trigger = 0.0f;         // index trigger, 0..1
    float squeeze = 0.0f;         // grip squeeze, 0..1
    glm::vec2 thumbstick{0.0f};   // primary 2D axis
    bool primaryButton = false;   // A / X
    bool secondaryButton = false; // B / Y
};

// Numeric values match XrHandJointEXT; this header stays OpenXR-free.
enum class XRHandJoint : int {
    Palm = 0,
    Wrist,
    ThumbMetacarpal,
    ThumbProximal,
    ThumbDistal,
    ThumbTip,
    IndexMetacarpal,
    IndexProximal,
    IndexIntermediate,
    IndexDistal,
    IndexTip,
    MiddleMetacarpal,
    MiddleProximal,
    MiddleIntermediate,
    MiddleDistal,
    MiddleTip,
    RingMetacarpal,
    RingProximal,
    RingIntermediate,
    RingDistal,
    RingTip,
    LittleMetacarpal,
    LittleProximal,
    LittleIntermediate,
    LittleDistal,
    LittleTip,
    Count
};

inline constexpr int kXRHandJointCount = static_cast<int>(XRHandJoint::Count);

struct XRHandJointState {
    bool valid = false;
    XRPose pose;
    float radius = 0.0f;
};

struct XRHandSkeletonState {
    bool active = false;
    std::array<XRHandJointState, kXRHandJointCount> joints{};

    const XRHandJointState& joint(XRHandJoint j) const {
        return joints[static_cast<int>(j)];
    }
};

// Group tags used to locate interactables without name lookups.
inline constexpr const char* kXRGrabbableGroup = "xr_grabbable";
inline constexpr const char* kXRTouchableGroup = "xr_touchable";
inline constexpr const char* kXROriginGroup = "xr_origin";          // the player rig
inline constexpr const char* kXRTeleportAreaGroup = "xr_teleport_area";

} // namespace saida
