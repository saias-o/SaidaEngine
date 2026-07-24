#pragma once

#include "graphics/VmaFwd.hpp"
#include "rhi/Format.hpp"
#include "rhi/TextureUsage.hpp"

#include <vulkan/vulkan.h>

#include <string>
#include <vector>


namespace saida {
class VulkanDevice;
}

namespace saida::rhi::vulkan {

struct RenderTextureDesc {
    rhi::Format format = rhi::Format::Undefined;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t layers = 1;
    uint32_t samples = 1;
    rhi::TextureUsage usage = rhi::TextureUsage::None;
    std::string memoryCategory;
};

class RenderTexture {
public:
    RenderTexture(VulkanDevice& device, const RenderTextureDesc& desc);
    ~RenderTexture();
    RenderTexture(const RenderTexture&) = delete;
    RenderTexture& operator=(const RenderTexture&) = delete;

    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }
    VkImageView layerView(uint32_t layer) const { return layerViews_[layer]; }

    rhi::Format format() const { return desc_.format; }
    VkExtent2D extent() const { return {desc_.width, desc_.height}; }
    uint32_t layers() const { return desc_.layers; }

private:
    VulkanDevice& device_;
    RenderTextureDesc desc_;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    std::vector<VkImageView> layerViews_;
    uint64_t trackedBytes_ = 0;
};

} // namespace saida::rhi::vulkan
