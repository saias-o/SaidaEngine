#include "xr/XrPlatform.hpp"   // must precede the XR headers (include order)
#include "xr/XrVulkanBinding.hpp"
#include "xr/XrInstance.hpp"

#include <stdexcept>

namespace saida::xr {

namespace {
void vkCheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string("OpenXR/Vulkan: ") + what + " failed");
}
} // namespace

VkInstance XrVulkanDeviceCreator::createInstance(const VkInstanceCreateInfo& ci) {
    return createVulkanInstance(xr_, ci);
}

VkPhysicalDevice XrVulkanDeviceCreator::pickPhysicalDevice(VkInstance instance) {
    return xr::pickPhysicalDevice(xr_, instance);
}

VkDevice XrVulkanDeviceCreator::createDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo& ci) {
    return createVulkanDevice(xr_, physicalDevice, ci);
}

VkInstance createVulkanInstance(const Instance& xr, const VkInstanceCreateInfo& ci) {
    // The runtime validates that our requested Vulkan version is supported.
    XrGraphicsRequirementsVulkan2KHR req{};
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR;
    auto getReq = reinterpret_cast<PFN_xrGetVulkanGraphicsRequirements2KHR>(
        xr.pfnGetVulkanGraphicsRequirements2);
    check(getReq(xr.handle(), xr.systemId(), &req), "xrGetVulkanGraphicsRequirements2KHR");

    XrVulkanInstanceCreateInfoKHR info{};
    info.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
    info.systemId = xr.systemId();
    info.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    info.vulkanCreateInfo = &ci;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult vkResult = VK_SUCCESS;
    auto create = reinterpret_cast<PFN_xrCreateVulkanInstanceKHR>(xr.pfnCreateVulkanInstance);
    check(create(xr.handle(), &info, &instance, &vkResult), "xrCreateVulkanInstanceKHR");
    vkCheck(vkResult, "vkCreateInstance (XR)");
    return instance;
}

VkPhysicalDevice pickPhysicalDevice(const Instance& xr, VkInstance vulkanInstance) {
    XrVulkanGraphicsDeviceGetInfoKHR info{};
    info.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
    info.systemId = xr.systemId();
    info.vulkanInstance = vulkanInstance;

    VkPhysicalDevice physical = VK_NULL_HANDLE;
    auto get = reinterpret_cast<PFN_xrGetVulkanGraphicsDevice2KHR>(xr.pfnGetVulkanGraphicsDevice2);
    check(get(xr.handle(), &info, &physical), "xrGetVulkanGraphicsDevice2KHR");
    return physical;
}

VkDevice createVulkanDevice(const Instance& xr, VkPhysicalDevice physicalDevice,
                            const VkDeviceCreateInfo& ci) {
    XrVulkanDeviceCreateInfoKHR info{};
    info.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
    info.systemId = xr.systemId();
    info.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    info.vulkanPhysicalDevice = physicalDevice;
    info.vulkanCreateInfo = &ci;

    VkDevice device = VK_NULL_HANDLE;
    VkResult vkResult = VK_SUCCESS;
    auto create = reinterpret_cast<PFN_xrCreateVulkanDeviceKHR>(xr.pfnCreateVulkanDevice);
    check(create(xr.handle(), &info, &device, &vkResult), "xrCreateVulkanDeviceKHR");
    vkCheck(vkResult, "vkCreateDevice (XR)");
    return device;
}

} // namespace saida::xr
