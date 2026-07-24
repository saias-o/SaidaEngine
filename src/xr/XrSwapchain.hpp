#pragma once

#include <vulkan/vulkan.h>
#include <openxr/openxr.h>

#include <vector>

namespace saida { class VulkanDevice; }

namespace saida::xr {

// XR color swapchain for one eye; wraps runtime images with VkImageViews.
class Swapchain {
public:
    Swapchain(VulkanDevice& device, XrSession session, int64_t format,
              uint32_t width, uint32_t height, VkSampleCountFlagBits samples);
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    uint32_t acquire();  // index of the image to render into this frame
    void wait();         // block until the runtime has released the image to us
    void release();      // hand the rendered image back to the compositor

    XrSwapchain handle() const { return swapchain_; }
    VkImage image(uint32_t i) const { return images_[i]; }
    VkImageView view(uint32_t i) const { return views_[i]; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    VkExtent2D extent() const { return {width_, height_}; }

private:
    VulkanDevice& device_;
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

} // namespace saida::xr
