#pragma once

#include <openxr/openxr.h>

namespace saida::xr {

class Instance;

// Optional XR_EXT_hand_tracking bridge. It owns one native tracker per hand and
// publishes the default 26-joint skeleton to XRInput once per XR frame. When the
// runtime does not expose the extension, Session simply does not create it.
class HandTracking {
public:
    HandTracking(Instance& instance, XrSession session);
    ~HandTracking();
    HandTracking(const HandTracking&) = delete;
    HandTracking& operator=(const HandTracking&) = delete;

    void sync(XrSpace baseSpace, XrTime time);

private:
    Instance& instance_;
    XrHandTrackerEXT trackers_[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};
    PFN_xrCreateHandTrackerEXT createHandTracker_ = nullptr;
    PFN_xrDestroyHandTrackerEXT destroyHandTracker_ = nullptr;
    PFN_xrLocateHandJointsEXT locateHandJoints_ = nullptr;
    bool lastActive_[2]{false, false};
};

} // namespace saida::xr
