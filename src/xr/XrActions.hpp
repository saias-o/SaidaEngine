#pragma once

#include <openxr/openxr.h>

#include <vector>

namespace saida::xr {

class Instance;

// OpenXR action-set input layer feeding saida::XRInput.
class Actions {
public:
    Actions(Instance& instance, XrSession session);
    ~Actions();
    Actions(const Actions&) = delete;
    Actions& operator=(const Actions&) = delete;

    // Locate hand poses in `space` and feed saida::XRInput.
    void sync(XrSession session, XrSpace space, XrTime time);

private:
    XrPath path(const char* str) const;
    XrAction makeAction(const char* name, XrActionType type);
    void suggest(const char* profile, const std::vector<XrActionSuggestedBinding>& bindings);

    XrInstance instance_ = XR_NULL_HANDLE;   // borrowed
    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrPath handPaths_[2]{};                   // /user/hand/left, /right

    XrAction gripPose_ = XR_NULL_HANDLE;
    XrAction aimPose_ = XR_NULL_HANDLE;
    XrAction squeeze_ = XR_NULL_HANDLE;
    XrAction trigger_ = XR_NULL_HANDLE;
    XrAction thumbstick_ = XR_NULL_HANDLE;
    XrAction primary_ = XR_NULL_HANDLE;
    XrAction secondary_ = XR_NULL_HANDLE;

    XrSpace gripSpace_[2]{};
    XrSpace aimSpace_[2]{};
};

} // namespace saida::xr
