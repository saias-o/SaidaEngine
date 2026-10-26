#pragma once

#include <openxr/openxr.h>

#include <vector>
#include <map>
#include <string>

namespace saida::xr {

class Instance;
class InputExtensions;

// OpenXR action-set input layer feeding saida::XRInput.
class Actions {
public:
    Actions(Instance& instance, XrSession session);
    ~Actions();
    Actions(const Actions&) = delete;
    Actions& operator=(const Actions&) = delete;

    // Locate hand poses in `space` and feed saida::XRInput.
    bool sync(XrSession session, XrSpace space, XrTime time);
    void deactivate(XrTime time);

private:
    void initialize(XrSession session);
    void release() noexcept;
    void bindProfiles();
    XrPath path(const char* str) const;
    XrAction makeAction(const char* name, XrActionType type);
    void suggest(const char* profile, const std::vector<XrActionSuggestedBinding>& bindings);

    XrInstance instance_ = XR_NULL_HANDLE; // borrowed
    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrPath handPaths_[2]{}; // /user/hand/left, /right

    XrAction gripPose_ = XR_NULL_HANDLE;
    XrAction aimPose_ = XR_NULL_HANDLE;
    XrAction squeeze_ = XR_NULL_HANDLE;
    XrAction trigger_ = XR_NULL_HANDLE;
    XrAction thumbstick_ = XR_NULL_HANDLE;
    XrAction primary_ = XR_NULL_HANDLE;
    XrAction secondary_ = XR_NULL_HANDLE;
    struct ProfileBindings {
        std::vector<XrActionSuggestedBinding> base;
        std::vector<XrActionSuggestedBinding> extensions;
    };
    std::map<std::string, ProfileBindings> bindings_;
    InputExtensions* inputExtensions_ = nullptr; // owned by the instance
    bool ownsInputSession_ = false;
    XrResult lastSyncResult_ = XR_SUCCESS;

    XrSpace gripSpace_[2]{};
    XrSpace aimSpace_[2]{};
};

} // namespace saida::xr
