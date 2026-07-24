#pragma once

#include "graphics/VmaFwd.hpp"
#include "rhi/vulkan/CommandEncoder.hpp"

#include <array>
#include <vector>

namespace saida {

class VulkanDevice;
class Window;

// Vulkan surface owns presentation and per-frame synchronization.
class Swapchain {
public:
    static constexpr uint32_t kFramesInFlight = 2;

    Swapchain(VulkanDevice& device, Window& window, bool vSync = false);
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    void waitFrame(uint32_t frame) const;
    // An out-of-date swapchain makes the caller rebuild targets and skip the frame.
    bool acquire(uint32_t frame, uint32_t& imageIndex);
    bool submitAndPresent(VkCommandBuffer cmd, uint32_t frame, uint32_t imageIndex);

    rhi::vulkan::CommandEncoder beginFrameCommands(uint32_t frame);
    bool submitAndPresent(rhi::vulkan::CommandEncoder& encoder, uint32_t frame,
                          uint32_t imageIndex);

    // Waits until the window has a non-zero size, then rebuilds the swap chain.
    void recreate();
    bool setVSync(bool enabled);
    bool vSync() const { return vSync_; }

    VkSwapchainKHR handle() const { return swapchain_; }
    VkExtent2D extent() const { return extent_; }
    VkSampleCountFlagBits samples() const { return samples_; }
    uint32_t imageCount() const { return static_cast<uint32_t>(images_.size()); }
    float aspectRatio() const { return extent_.width / static_cast<float>(extent_.height); }

    VkFormat colorFormat() const { return imageFormat_; }
    VkFormat depthFormat() const { return depthFormat_; }

    // Per-image present target (single-sample).
    VkImage image(uint32_t i) const { return images_[i]; }
    VkImageView imageView(uint32_t i) const { return imageViews_[i]; }

    // Multisampled color target (rendered into, then resolved to the swap image).
    // Null views/images when samples == 1 (render straight to the swap image).
    VkImage msaaColorImage() const { return colorImage_; }
    VkImageView msaaColorView() const { return colorImageView_; }

    VkImage depthImage() const { return depthImage_; }
    VkImageView depthView() const { return depthImageView_; }

private:
    void createSwapchain();
    void createImageViews();
    void createColorResources();
    void createDepthResources();
    void createRenderFinishedSemaphores();
    void createFrameSync();
    void cleanup();

    VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available) const;
    VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& available) const;
    VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;

    VulkanDevice& device_;
    Window& window_;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> images_;
    std::vector<VkImageView> imageViews_;
    VkFormat imageFormat_{};
    VkFormat depthFormat_{};
    VkExtent2D extent_{};
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    bool vSync_ = false;

    // Multisampled color target, resolved into the swap-chain image for present.
    VkImage colorImage_ = VK_NULL_HANDLE;
    VmaAllocation colorAllocation_ = VK_NULL_HANDLE;
    VkImageView colorImageView_ = VK_NULL_HANDLE;

    VkImage depthImage_ = VK_NULL_HANDLE;
    VmaAllocation depthAllocation_ = VK_NULL_HANDLE;
    VkImageView depthImageView_ = VK_NULL_HANDLE;

    // renderFinished: one per swap-chain image (never reused while its present
    // is pending). imageAvailable + fences: one per frame in flight.
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkFence> inFlightFences_;

    // Frame command buffers (one per frame in flight), allocated lazily from the
    // device pool on first beginFrameCommands. Freed with the pool.
    std::array<VkCommandBuffer, kFramesInFlight> frameCommandBuffers_{};
};

} // namespace saida
