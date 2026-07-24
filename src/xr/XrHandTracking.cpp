#include "xr/XrPlatform.hpp" // must precede OpenXR platform-dependent headers
#include "xr/XrHandTracking.hpp"

#include "xr/XrInstance.hpp"
#include "xr/XrMath.hpp"
#include "core/Log.hpp"
#include "xr/toolkit/XRInput.hpp"
#include "xr/toolkit/XRTypes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

namespace saida::xr {

namespace {

template <typename T>
T loadProc(XrInstance instance, const char* name) {
    PFN_xrVoidFunction function = nullptr;
    check(xrGetInstanceProcAddr(instance, name, &function), name);
    if (!function)
        throw std::runtime_error(std::string("OpenXR: missing extension function ") + name);
    return reinterpret_cast<T>(function);
}

float inverseDistance(float distance, float fullyPressed, float released) {
    if (released <= fullyPressed) return 0.0f;
    return std::clamp((released - distance) / (released - fullyPressed), 0.0f, 1.0f);
}

} // namespace

HandTracking::HandTracking(Instance& instance, XrSession session)
    : instance_(instance) {
    createHandTracker_ = loadProc<PFN_xrCreateHandTrackerEXT>(
        instance.handle(), "xrCreateHandTrackerEXT");
    destroyHandTracker_ = loadProc<PFN_xrDestroyHandTrackerEXT>(
        instance.handle(), "xrDestroyHandTrackerEXT");
    locateHandJoints_ = loadProc<PFN_xrLocateHandJointsEXT>(
        instance.handle(), "xrLocateHandJointsEXT");

    static_assert(kXRHandJointCount == XR_HAND_JOINT_COUNT_EXT,
                  "Toolkit and OpenXR hand skeletons must have the same joint count");

    try {
        for (int i = 0; i < 2; ++i) {
            XrHandTrackerCreateInfoEXT createInfo{};
            createInfo.type = XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT;
            createInfo.hand = i == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
            createInfo.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
            check(createHandTracker_(session, &createInfo, &trackers_[i]),
                  "xrCreateHandTrackerEXT");
        }
    } catch (...) {
        for (XrHandTrackerEXT& tracker : trackers_)
            if (tracker != XR_NULL_HANDLE) {
                destroyHandTracker_(tracker);
                tracker = XR_NULL_HANDLE;
            }
        throw;
    }
}

HandTracking::~HandTracking() {
    for (XrHandTrackerEXT tracker : trackers_)
        if (tracker != XR_NULL_HANDLE) destroyHandTracker_(tracker);
}

void HandTracking::sync(XrSpace baseSpace, XrTime time) {
    if (time <= 0 || baseSpace == XR_NULL_HANDLE) return;

    for (int handIndex = 0; handIndex < 2; ++handIndex) {
        std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> locations{};

        XrHandJointLocationsEXT jointLocations{};
        jointLocations.type = XR_TYPE_HAND_JOINT_LOCATIONS_EXT;
        jointLocations.jointCount = static_cast<uint32_t>(locations.size());
        jointLocations.jointLocations = locations.data();

        XrHandJointsLocateInfoEXT locateInfo{};
        locateInfo.type = XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT;
        locateInfo.baseSpace = baseSpace;
        locateInfo.time = time;

        const XRHand hand = handIndex == 0 ? XRHand::Left : XRHand::Right;
        XRHandSkeletonState skeleton;
        if (XR_FAILED(locateHandJoints_(trackers_[handIndex], &locateInfo, &jointLocations)) ||
            jointLocations.isActive != XR_TRUE) {
            if (lastActive_[handIndex])
                Log::info("XR ", handIndex == 0 ? "left" : "right", " hand tracking lost");
            lastActive_[handIndex] = false;
            XRInput::submitSkeleton(hand, skeleton);
            continue;
        }

        if (!lastActive_[handIndex])
            Log::info("XR ", handIndex == 0 ? "left" : "right", " hand tracking active");
        lastActive_[handIndex] = true;
        skeleton.active = true;
        for (int jointIndex = 0; jointIndex < kXRHandJointCount; ++jointIndex) {
            const auto& source = locations[static_cast<size_t>(jointIndex)];
            auto& target = skeleton.joints[static_cast<size_t>(jointIndex)];
            const bool positionValid =
                (source.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
            const bool orientationValid =
                (source.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
            target.valid = positionValid && orientationValid;
            target.pose.tracked = target.valid;
            if (positionValid) target.pose.position = toGlm(source.pose.position);
            if (orientationValid) target.pose.orientation = toGlm(source.pose.orientation);
            target.radius = source.radius;
        }
        XRInput::submitSkeleton(hand, skeleton);

        // Hand-only mode has no controller actions. Derive the existing toolkit's
        // grip/trigger/squeeze state from the skeleton so XRController-based grab
        // and touch behaviours continue to work without special cases.
        const auto& palm = skeleton.joint(XRHandJoint::Palm);
        const auto& thumbTip = skeleton.joint(XRHandJoint::ThumbTip);
        const auto& indexTip = skeleton.joint(XRHandJoint::IndexTip);
        if (!palm.valid) continue;

        XRHandState handState;
        handState.active = true;
        handState.grip = palm.pose;
        handState.aim = palm.pose;

        if (thumbTip.valid && indexTip.valid) {
            const float pinchDistance = glm::distance(
                thumbTip.pose.position, indexTip.pose.position);
            handState.trigger = inverseDistance(pinchDistance, 0.018f, 0.065f);
        }

        const XRHandJoint fingerTips[] = {
            XRHandJoint::IndexTip, XRHandJoint::MiddleTip,
            XRHandJoint::RingTip, XRHandJoint::LittleTip};
        float tipDistance = 0.0f;
        int validTips = 0;
        for (XRHandJoint tip : fingerTips) {
            const auto& joint = skeleton.joint(tip);
            if (!joint.valid) continue;
            tipDistance += glm::distance(joint.pose.position, palm.pose.position);
            ++validTips;
        }
        if (validTips > 0)
            handState.squeeze = inverseDistance(
                tipDistance / static_cast<float>(validTips), 0.055f, 0.14f);

        XRInput::submitHand(hand, handState);
    }
}

} // namespace saida::xr
