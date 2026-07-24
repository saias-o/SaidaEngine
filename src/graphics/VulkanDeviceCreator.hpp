#pragma once

#include <vulkan/vulkan.h>

namespace saida {

// Interface used by VulkanDevice to delegate Vulkan instance, physical device
// and logical device creation in non-desktop environments (like OpenXR).
class VulkanDeviceCreator {
public:
    virtual ~VulkanDeviceCreator() = default;

    // Creates the VkInstance given the engine's standard create info.
    // The creator is responsible for calling the custom creation function and returning the handle.
    virtual VkInstance createInstance(const VkInstanceCreateInfo& ci) = 0;

    // Selects the VkPhysicalDevice.
    virtual VkPhysicalDevice pickPhysicalDevice(VkInstance instance) = 0;

    // Creates the VkDevice given the selected physical device and engine's device create info.
    virtual VkDevice createDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo& ci) = 0;

    // Returns whether this device configuration requires/supports a GLFW window surface.
    virtual bool requiresSurface() const = 0;
};

} // namespace saida
