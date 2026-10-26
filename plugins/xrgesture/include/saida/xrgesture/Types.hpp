#pragma once

#include "xr/toolkit/XRTypes.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <functional>

namespace saida::xrgesture {

enum class XRMicrogesture : uint8_t { SwipeLeft, SwipeRight, SwipeForward, SwipeBackward, ThumbTap, Count };
inline constexpr uint32_t xrMicrogestureBit(XRMicrogesture gesture) {
    return uint32_t{1} << static_cast<uint8_t>(gesture);
}
struct XRNativeMicrogestureState {
    bool supported = false;
    bool active = false;
    uint32_t downMask = 0;
    uint32_t pressedMask = 0;
};
struct RuntimeInputState {
    bool focused = false;
    std::array<XRNativeMicrogestureState, kXRHandCount> hands{};
};

using GestureId = uint32_t;
namespace gesture {
inline constexpr GestureId SwipeLeft = 1, SwipeRight = 2, SwipeForward = 3;
inline constexpr GestureId SwipeBackward = 4, ThumbTap = 5;
inline constexpr GestureId Pinch = 6, Grab = 7, OpenPalm = 8, Point = 9;
inline constexpr GestureId FirstCustom = 1024;
} // namespace gesture

enum class Phase { Started, Updated, Ended, Cancelled, Performed };
enum class Source { NativeMeta, Skeleton, Custom };
enum class MicrogestureMode { NativeOnly, NativePreferred, SkeletonOnly };

struct HandSample {
    XRHandSkeletonState skeleton;
    XRNativeMicrogestureState native;
};

struct Frame {
    double time = 0; // monotonic, unscaled seconds; one update per sample
    bool focused = false;
    XRPose head;
    std::array<HandSample, kXRHandCount> hands{};
};

struct Event {
    GestureId gesture = 0;
    XRHand hand = XRHand::Left;
    Phase phase = Phase::Performed;
    Source source = Source::Custom;
    double time = 0;
    float strength = 0;
};

// Borrowed until the next update/reset, keeping the C++17 engine contract.
struct EventRange {
    const Event* data = nullptr;
    size_t count = 0;
    const Event* begin() const { return data; }
    const Event* end() const { return count ? data + count : data; }
    size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const Event& operator[](size_t index) const { return data[index]; }
};

struct Observation {
    bool valid = false;
    float strength = 0;
};

enum class Recognition { Continuous, Discrete };
struct Rule {
    GestureId id = 0;
    // The full frame supports bimanual and application-specific recognizers.
    std::function<Observation(const Frame&, XRHand)> evaluate;
    std::function<void(XRHand)> onReset; // discard temporal history on invalidation
    Recognition recognition = Recognition::Continuous;
    float enter = .8f;
    float leave = .5f;
    double holdSeconds = .06;
    double cooldownSeconds = .12;
    Source source = Source::Custom;
};

inline bool finite(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
inline bool validHand(XRHand hand) {
    return hand == XRHand::Left || hand == XRHand::Right;
}
inline bool finite(const glm::quat& q) {
    const float norm = glm::dot(q, q);
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w) &&
           std::isfinite(norm) && norm > .0001f;
}
inline bool valid(const XRHandJointState& joint) {
    return joint.valid && joint.pose.tracked && finite(joint.pose.position) &&
           finite(joint.pose.orientation) && std::isfinite(joint.radius) && joint.radius >= 0;
}

inline const char* name(GestureId id) {
    switch (id) {
    case gesture::SwipeLeft:
        return "swipe-left";
    case gesture::SwipeRight:
        return "swipe-right";
    case gesture::SwipeForward:
        return "swipe-forward";
    case gesture::SwipeBackward:
        return "swipe-backward";
    case gesture::ThumbTap:
        return "thumb-tap";
    case gesture::Pinch:
        return "pinch";
    case gesture::Grab:
        return "grab";
    case gesture::OpenPalm:
        return "open-palm";
    case gesture::Point:
        return "point";
    default:
        return "custom";
    }
}
inline const char* name(Source source) {
    switch (source) {
    case Source::NativeMeta:
        return "meta-openxr";
    case Source::Skeleton:
        return "skeleton-heuristic";
    default:
        return "custom";
    }
}

} // namespace saida::xrgesture
