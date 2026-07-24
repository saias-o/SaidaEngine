#include "xr/XrPlatform.hpp"   // must precede the XR headers (include order)
#include "xr/XrInstance.hpp"

#include "core/Log.hpp"

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace saida::xr {

namespace {

bool extensionAvailable(const std::vector<XrExtensionProperties>& extensions,
                        const char* name) {
    for (const auto& extension : extensions)
        if (std::strcmp(extension.extensionName, name) == 0) return true;
    return false;
}

const char* resultName(XrInstance instance, XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, r, buf)))
        return buf;
    std::snprintf(buf, sizeof(buf), "XrResult(%d)", static_cast<int>(r));
    return buf;
}

// Create an instance + resolve the HMD system. Returns true on full success.
bool createInstanceAndSystem(XrInstance& outInstance, XrSystemId& outSystem,
                             bool& outHandTrackingExtension) {
    uint32_t extensionCount = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(
            nullptr, 0, &extensionCount, nullptr))) return false;
    XrExtensionProperties extensionTemplate{};
    extensionTemplate.type = XR_TYPE_EXTENSION_PROPERTIES;
    std::vector<XrExtensionProperties> extensions(extensionCount, extensionTemplate);
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(
            nullptr, extensionCount, &extensionCount, extensions.data()))) return false;

    if (!extensionAvailable(extensions, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
        return false;

    std::vector<const char*> enabledExtensions{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    outHandTrackingExtension = extensionAvailable(
        extensions, XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    if (outHandTrackingExtension)
        enabledExtensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);

    XrInstanceCreateInfo ci{};
    ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
    std::strncpy(ci.applicationInfo.applicationName, "SaidaEngine",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    std::strncpy(ci.applicationInfo.engineName, "SaidaEngine",
                 XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    ci.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    ci.enabledExtensionNames = enabledExtensions.data();

    XrResult r = xrCreateInstance(&ci, &outInstance);
    if (XR_FAILED(r)) return false;  // no runtime / extension unsupported

    XrSystemGetInfo sysInfo{};
    sysInfo.type = XR_TYPE_SYSTEM_GET_INFO;
    sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(outInstance, &sysInfo, &outSystem);
    if (XR_FAILED(r)) {  // runtime present but no headset connected
        xrDestroyInstance(outInstance);
        outInstance = XR_NULL_HANDLE;
        return false;
    }
    return true;
}

} // namespace

void check(XrResult r, const char* what) {
    if (XR_FAILED(r))
        throw std::runtime_error(std::string("OpenXR: ") + what + " failed: " +
                                 resultName(XR_NULL_HANDLE, r));
}

Instance::Instance() {
    if (!createInstanceAndSystem(instance_, systemId_, handTrackingSupported_))
        throw std::runtime_error("OpenXR: failed to create instance / find HMD system");

    XrInstanceProperties props{};
    props.type = XR_TYPE_INSTANCE_PROPERTIES;
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance_, &props)))
        Log::info("OpenXR runtime: ", props.runtimeName);

    XrSystemHandTrackingPropertiesEXT handProperties{};
    handProperties.type = XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT;
    XrSystemProperties sysProps{};
    sysProps.type = XR_TYPE_SYSTEM_PROPERTIES;
    if (handTrackingSupported_) sysProps.next = &handProperties;
    if (XR_SUCCEEDED(xrGetSystemProperties(instance_, systemId_, &sysProps))) {
        Log::info("XR system: ", sysProps.systemName);
        handTrackingSupported_ = handTrackingSupported_ &&
                                 handProperties.supportsHandTracking == XR_TRUE;
    } else {
        handTrackingSupported_ = false;
    }
    Log::info("XR hand tracking ", handTrackingSupported_ ? "supported" : "unavailable");

    loadFunctions();
}

Instance::~Instance() {
    if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
}

void Instance::loadFunctions() {
    auto load = [this](const char* name, PFN_xrVoidFunction& out) {
        check(xrGetInstanceProcAddr(instance_, name, &out), name);
    };
    load("xrGetVulkanGraphicsRequirements2KHR", pfnGetVulkanGraphicsRequirements2);
    load("xrCreateVulkanInstanceKHR", pfnCreateVulkanInstance);
    load("xrGetVulkanGraphicsDevice2KHR", pfnGetVulkanGraphicsDevice2);
    load("xrCreateVulkanDeviceKHR", pfnCreateVulkanDevice);
}

} // namespace saida::xr
