#include "xr/XrPlatform.hpp"   // must precede the XR headers (include order)
#include "xr/XrSwapchain.hpp"
#include "xr/XrInstance.hpp"   // check()

#include "graphics/VulkanDevice.hpp"

namespace saida::xr {

Swapchain::Swapchain(VulkanDevice& device, XrSession session, int64_t format,
                     uint32_t width, uint32_t height, VkSampleCountFlagBits samples)
    : device_(device), width_(width), height_(height) {
    XrSwapchainCreateInfo ci{};
    ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = format;
    ci.sampleCount = static_cast<uint32_t>(samples);
    ci.width = width;
    ci.height = height;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    check(xrCreateSwapchain(session, &ci, &swapchain_), "xrCreateSwapchain");

    uint32_t count = 0;
    check(xrEnumerateSwapchainImages(swapchain_, 0, &count, nullptr),
          "xrEnumerateSwapchainImages(count)");
    XrSwapchainImageVulkan2KHR imageTemplate{};
    imageTemplate.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    std::vector<XrSwapchainImageVulkan2KHR> xrImages(count, imageTemplate);
    check(xrEnumerateSwapchainImages(
              swapchain_, count, &count,
              reinterpret_cast<XrSwapchainImageBaseHeader*>(xrImages.data())),
          "xrEnumerateSwapchainImages");

    images_.resize(count);
    views_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        images_[i] = xrImages[i].image;
        views_[i] = device_.createImageView(images_[i],
            static_cast<VkFormat>(format), VK_IMAGE_ASPECT_COLOR_BIT);
    }
}

Swapchain::~Swapchain() {
    for (VkImageView v : views_)
        vkDestroyImageView(device_.device(), v, nullptr);
    if (swapchain_ != XR_NULL_HANDLE) xrDestroySwapchain(swapchain_);
}

uint32_t Swapchain::acquire() {
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo info{};
    info.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    check(xrAcquireSwapchainImage(swapchain_, &info, &index), "xrAcquireSwapchainImage");
    return index;
}

void Swapchain::wait() {
    XrSwapchainImageWaitInfo info{};
    info.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    info.timeout = XR_INFINITE_DURATION;
    check(xrWaitSwapchainImage(swapchain_, &info), "xrWaitSwapchainImage");
}

void Swapchain::release() {
    XrSwapchainImageReleaseInfo info{};
    info.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    check(xrReleaseSwapchainImage(swapchain_, &info), "xrReleaseSwapchainImage");
}

} // namespace saida::xr
