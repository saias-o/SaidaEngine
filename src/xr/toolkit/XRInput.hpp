#pragma once

#include "xr/toolkit/XRTypes.hpp"

namespace saida {

// Global XR input service: sampled hand/head state plus derived button edges.
class XRInput {
public:
    // ---- Consumer API ----
    static const XRHandState& hand(XRHand h);
    static const XRHandSkeletonState& skeleton(XRHand h);
    static bool anyActive();
    static bool handTrackingActive(XRHand h);

    // Head (HMD) pose in world space.
    static const XRPose& head();

    // Grip squeeze: level + edges.
    static bool squeezeDown(XRHand h);
    static bool squeezePressed(XRHand h);
    static bool squeezeReleased(XRHand h);

    // Index trigger: level + edges.
    static bool triggerDown(XRHand h);
    static bool triggerPressed(XRHand h);
    static bool triggerReleased(XRHand h);

    // The analog value beyond which a squeeze/trigger counts as "down".
    static void setPressThreshold(float t);
    static float pressThreshold();

    // Snapshot the current state as "previous" so edges can be derived; call once
    // at the start of an XR frame, before submitHand().
    static void beginFrame();
    // Provide a freshly polled state for one hand this frame.
    static void submitHand(XRHand h, const XRHandState& state);
    static void submitSkeleton(XRHand h, const XRHandSkeletonState& state);
    // Mark a hand as not present this frame (e.g. controller dropped).
    static void clearHand(XRHand h);
    // Provide the head pose (world space) for this frame.
    static void submitHead(const XRPose& pose);
};

} // namespace saida
