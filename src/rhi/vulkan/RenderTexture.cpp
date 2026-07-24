#include "rhi/vulkan/RenderTexture.hpp"

#include "graphics/MemoryProfiler.hpp"
#include "graphics/VulkanDevice.hpp"
#include "rhi/vulkan/Format.hpp"

#include "vk_mem_alloc.h"

#include <stdexcept>

namespace saida::rhi::vulkan {

namespace {

VkImageUsageFlags toVk(rhi::TextureUsage usage) {
    VkImageUsageFlags flags = 0;
    if (hasUsage(usage, rhi::TextureUsage::Sampled)) flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (hasUsage(usage, rhi::TextureUsage::Storage)) flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (hasUsage(usage, rhi::TextureUsage::ColorAttachment))
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (hasUsage(usage, rhi::TextureUsage::DepthAttachment))
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (hasUsage(usage, rhi::TextureUsage::CopySrc)) flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (hasUsage(usage, rhi::TextureUsage::CopyDst)) flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (hasUsage(usage, rhi::TextureUsage::Transient))
        flags |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    return flags;
}

} // namespace

RenderTexture::RenderTexture(VulkanDevice& device, const RenderTextureDesc& desc)
    : device_(device), desc_(desc) {
    const VkFormat format = toVk(desc_.format);
    const bool is3d = desc_.depth > 1;
    const VkImageAspectFlags aspect =
        isDepthFormat(desc_.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.extent = {desc_.width, desc_.height, desc_.depth};
    info.mipLevels = 1;
    info.arrayLayers = desc_.layers;
    info.format = format;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage = toVk(desc_.usage);
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.samples = static_cast<VkSampleCountFlagBits>(desc_.samples);

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(device_.allocator(), &info, &alloc, &image_, &allocation_, nullptr) !=
        VK_SUCCESS)
        throw std::runtime_error("failed to create render texture");

    auto makeView = [&](VkImageViewType type, uint32_t baseLayer, uint32_t layerCount) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = image_;
        vi.viewType = type;
        vi.format = format;
        vi.subresourceRange = {aspect, 0, 1, baseLayer, layerCount};
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(device_.device(), &vi, nullptr, &view) != VK_SUCCESS)
            throw std::runtime_error("failed to create render texture view");
        return view;
    };

    const VkImageViewType wholeType = is3d ? VK_IMAGE_VIEW_TYPE_3D
                                    : desc_.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                       : VK_IMAGE_VIEW_TYPE_2D;
    view_ = makeView(wholeType, 0, desc_.layers);
    if (desc_.layers > 1) {
        layerViews_.reserve(desc_.layers);
        for (uint32_t i = 0; i < desc_.layers; ++i)
            layerViews_.push_back(makeView(VK_IMAGE_VIEW_TYPE_2D, i, 1));
    }

    if (!desc_.memoryCategory.empty()) {
        trackedBytes_ = static_cast<uint64_t>(desc_.width) * desc_.height * desc_.depth *
                        desc_.layers * desc_.samples * bytesPerTexel(desc_.format);
        MemoryProfiler::registerAllocation(desc_.memoryCategory, trackedBytes_);
    }
}

RenderTexture::~RenderTexture() {
    if (trackedBytes_)
        MemoryProfiler::unregisterAllocation(desc_.memoryCategory, trackedBytes_);
    for (VkImageView view : layerViews_)
        vkDestroyImageView(device_.device(), view, nullptr);
    vkDestroyImageView(device_.device(), view_, nullptr);
    vmaDestroyImage(device_.allocator(), image_, allocation_);
}

} // namespace saida::rhi::vulkan
