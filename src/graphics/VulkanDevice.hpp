#pragma once

#include "graphics/VmaFwd.hpp"
#include "rhi/Capabilities.hpp"

#include <vulkan/vulkan.h>

#include <functional>
#include <optional>
#include <vector>

namespace saida::rhi::vulkan {
class CommandEncoder;
}

namespace saida {

class Window;
class VulkanDeviceCreator;

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;
    std::optional<uint32_t> computeFamily;          // a compute-capable family
    bool computeIsDedicated = false;                // compute family != graphics
    bool isComplete() const { return graphicsFamily.has_value() && presentFamily.has_value(); }
};

struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

// Central Vulkan/GPU handle borrowed by the renderer and resource wrappers.
class VulkanDevice {
public:
    // Standard and Custom/XR constructor. Passing a non-null creator delegates
    // Vulkan instance/device creation (e.g., OpenXR). No GLFW surface is created
    // if creator->requiresSurface() is false.
    explicit VulkanDevice(Window& window, VulkanDeviceCreator* creator = nullptr);
    ~VulkanDevice();
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    VkInstance instance() const { return instance_; }
    VkDevice device() const { return device_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkSurfaceKHR surface() const { return surface_; }
    VkQueue graphicsQueue() const { return graphicsQueue_; }
    VkQueue presentQueue() const { return presentQueue_; }
    // Async-compute queue. Falls back to the graphics queue when the GPU has no
    // dedicated compute family (caps.dedicatedComputeQueue tells which it is).
    VkQueue computeQueue() const { return computeQueue_; }
    uint32_t computeQueueFamily() const { return computeFamily_; }
    VkCommandPool commandPool() const { return commandPool_; }
    VkCommandPool computeCommandPool() const { return computeCommandPool_; }
    VmaAllocator allocator() const { return allocator_; }
    VkPipelineCache pipelineCache() const { return pipelineCache_; }
    VkSampleCountFlagBits maxUsableSampleCount() const;
    float maxAnisotropy() const;

    // Supported/enabled GPU features.
    const rhi::Capabilities& capabilities() const { return capabilities_; }

    QueueFamilyIndices findQueueFamilies() const { return findQueueFamilies(physicalDevice_); }
    SwapChainSupportDetails querySwapChainSupport() const { return querySwapChainSupport(physicalDevice_); }

    VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates,
                                 VkImageTiling tiling, VkFormatFeatureFlags features) const;
    VkFormat findDepthFormat() const;

    VkImageView createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect) const;

    // Waits for a one-shot batch so callers can safely use its result immediately.
    void withSingleTimeEncoder(const std::function<void(rhi::vulkan::CommandEncoder&)>& fn) const;

    VkCommandBuffer beginSingleTimeCommands() const;
    void endSingleTimeCommands(VkCommandBuffer cmd) const;

    // Blocks until the GPU is idle (target recreation, shutdown).
    void waitIdle() const { vkDeviceWaitIdle(device_); }

private:
    void createInstance();
    void setupDebugMessenger();
    void createSurface(Window& window);
    void pickPhysicalDevice();
    void queryCapabilities();
    void createLogicalDevice();
    void createCommandPools();
    void createAllocator();
    void createPipelineCache();
    void savePipelineCache() const;

    bool checkValidationLayerSupport() const;
    std::vector<const char*> requiredInstanceExtensions() const;
    bool isDeviceSuitable(VkPhysicalDevice dev) const;
    bool checkDeviceExtensionSupport(VkPhysicalDevice dev) const;
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice dev) const;
    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice dev) const;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    VkQueue computeQueue_ = VK_NULL_HANDLE;
    uint32_t computeFamily_ = 0;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandPool computeCommandPool_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
    rhi::Capabilities capabilities_;
    bool validationEnabled_ = false;
    VulkanDeviceCreator* creator_ = nullptr;  // non-null → custom/OpenXR-driven init
};

} // namespace saida
