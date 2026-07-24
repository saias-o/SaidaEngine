#pragma once

#include <vulkan/vulkan.h>
#include <openxr/openxr.h>

namespace saida::xr {

// Throw a std::runtime_error (with the XrResult name) when `r` is a failure.
void check(XrResult r, const char* what);
inline bool succeeded(XrResult r) { return r >= 0; }

// OpenXR runtime handle plus cached XR_KHR_vulkan_enable2 entry points.
class Instance {
public:
    Instance();
    ~Instance();
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    XrInstance handle() const { return instance_; }
    XrSystemId systemId() const { return systemId_; }
    bool handTrackingSupported() const { return handTrackingSupported_; }

    // Cached vulkan_enable2 procs, stored as the generic XR proc type (core, so
    // this header needs no platform include). XrVulkanBinding casts them back.
    PFN_xrVoidFunction pfnGetVulkanGraphicsRequirements2 = nullptr;
    PFN_xrVoidFunction pfnCreateVulkanInstance = nullptr;
    PFN_xrVoidFunction pfnGetVulkanGraphicsDevice2 = nullptr;
    PFN_xrVoidFunction pfnCreateVulkanDevice = nullptr;

private:
    void loadFunctions();

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    bool handTrackingSupported_ = false;
};

} // namespace saida::xr
