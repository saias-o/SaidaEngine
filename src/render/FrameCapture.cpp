#include "render/FrameCapture.hpp"

#ifndef SAIDA_RHI_WEBGPU

#include "core/Log.hpp"
#include "core/PngWriter.hpp"
#include "graphics/Buffer.hpp"
#include "graphics/Swapchain.hpp"
#include "graphics/VulkanDevice.hpp"
#include "rhi/vulkan/CommandEncoder.hpp"

#include <vulkan/vulkan.h>

#include <cstring>
#include <vector>

namespace saida {

namespace {

// Channel order of the presented image. The swapchain prefers
// B8G8R8A8_SRGB (Swapchain::chooseSurfaceFormat) but falls back to whatever the
// surface offers first, so the layout is resolved from the actual format rather
// than assumed. A format outside this set is refused rather than written with
// swapped channels.
enum class ChannelOrder { Rgba, Bgra, Unsupported };

ChannelOrder channelOrderOf(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        return ChannelOrder::Rgba;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return ChannelOrder::Bgra;
    default:
        return ChannelOrder::Unsupported;
    }
}

const char* formatName(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
    default: return "unsupported";
    }
}

} // namespace

FrameCapture::FrameCapture(VulkanDevice& device, Swapchain& swapchain)
    : device_(device), swapchain_(swapchain) {}

FrameCapture::~FrameCapture() = default;

bool FrameCapture::request(const std::string& pngPath, std::string& error) {
    if (pending_) {
        error = "a frame capture is already pending";
        return false;
    }
    if (!swapchain_.supportsCapture()) {
        error = "this surface does not allow reading the presented image back "
                "(no TRANSFER_SRC on the swapchain images)";
        return false;
    }
    if (channelOrderOf(swapchain_.colorFormat()) == ChannelOrder::Unsupported) {
        error = std::string("unsupported swapchain format for capture: ") +
                formatName(swapchain_.colorFormat());
        return false;
    }

    const VkExtent2D extent = swapchain_.extent();
    if (extent.width == 0 || extent.height == 0) {
        error = "the swapchain has no extent yet";
        return false;
    }

    const uint64_t bytes = static_cast<uint64_t>(extent.width) * extent.height * 4;
    staging_ = std::make_unique<Buffer>(device_, bytes, rhi::BufferUsage::TransferDst,
                                        MemoryUsage::HostVisible);
    if (staging_->mapped() == nullptr) {
        staging_.reset();
        error = "the capture staging buffer is not host-visible";
        return false;
    }

    pngPath_ = pngPath;
    width_ = extent.width;
    height_ = extent.height;
    copied_ = false;
    pending_ = true;
    return true;
}

void FrameCapture::recordCopy(rhi::vulkan::CommandEncoder& encoder, uint32_t imageIndex) {
    if (!pending_ || copied_) return;

    // A resize between request() and this frame would make the staging buffer
    // the wrong size. Drop the capture loudly rather than write a torn image.
    const VkExtent2D extent = swapchain_.extent();
    if (extent.width != width_ || extent.height != height_) {
        Log::error("[capture] the swapchain resized before the frame was captured "
                   "(", width_, "x", height_, " -> ", extent.width, "x", extent.height, ")");
        pending_ = false;
        staging_.reset();
        return;
    }

    VkImage image = swapchain_.image(imageIndex);
    encoder.transition(image, rhi::ResourceState::ColorAttachment, rhi::ResourceState::CopySrc);
    encoder.copyTextureToBuffer(image, *staging_, width_, height_);
    encoder.transition(image, rhi::ResourceState::CopySrc, rhi::ResourceState::ColorAttachment);
    copied_ = true;
}

bool FrameCapture::resolve(std::string& error) {
    if (!pending_) {
        error = "no frame capture is pending";
        return false;
    }
    if (!copied_) {
        error = "the frame was never recorded (no frame was drawn after the request)";
        pending_ = false;
        staging_.reset();
        return false;
    }

    const size_t pixelCount = static_cast<size_t>(width_) * height_;
    std::vector<uint8_t> rgba(pixelCount * 4);
    const auto* source = static_cast<const uint8_t*>(staging_->mapped());

    // The presented image is opaque (COMPOSITE_ALPHA_OPAQUE), so its alpha
    // channel carries nothing; a screenshot forces it rather than inheriting
    // whatever the surface left there.
    if (channelOrderOf(swapchain_.colorFormat()) == ChannelOrder::Bgra) {
        for (size_t i = 0; i < pixelCount; ++i) {
            rgba[i * 4 + 0] = source[i * 4 + 2];
            rgba[i * 4 + 1] = source[i * 4 + 1];
            rgba[i * 4 + 2] = source[i * 4 + 0];
            rgba[i * 4 + 3] = 255;
        }
    } else {
        for (size_t i = 0; i < pixelCount; ++i) {
            rgba[i * 4 + 0] = source[i * 4 + 0];
            rgba[i * 4 + 1] = source[i * 4 + 1];
            rgba[i * 4 + 2] = source[i * 4 + 2];
            rgba[i * 4 + 3] = 255;
        }
    }

    const bool ok = writePngRGBA8(pngPath_, rgba.data(), width_, height_, error);
    pending_ = false;
    copied_ = false;
    staging_.reset();
    return ok;
}

} // namespace saida

#endif // SAIDA_RHI_WEBGPU
