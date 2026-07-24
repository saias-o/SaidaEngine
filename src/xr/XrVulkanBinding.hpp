#pragma once

#include <vulkan/vulkan.h>
#include "graphics/VulkanDeviceCreator.hpp"

namespace saida::xr {

class Instance;

// Implementation of VulkanDeviceCreator that delegates to OpenXR Vulkan binding functions.
class XrVulkanDeviceCreator : public VulkanDeviceCreator {
public:
    explicit XrVulkanDeviceCreator(const Instance& xrInstance) : xr_(xrInstance) {}

    VkInstance createInstance(const VkInstanceCreateInfo& ci) override;
    VkPhysicalDevice pickPhysicalDevice(VkInstance instance) override;
    VkDevice createDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo& ci) override;
    bool requiresSurface() const override { return false; }

private:
    const Instance& xr_;
};

VkInstance createVulkanInstance(const Instance& xr, const VkInstanceCreateInfo& ci);
VkPhysicalDevice pickPhysicalDevice(const Instance& xr, VkInstance vulkanInstance);
VkDevice createVulkanDevice(const Instance& xr, VkPhysicalDevice physicalDevice,
                            const VkDeviceCreateInfo& ci);

} // namespace saida::xr
