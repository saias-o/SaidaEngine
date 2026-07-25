#pragma once

#ifdef SAIDA_RHI_WEBGPU

#include "rhi/webgpu/Texture.hpp"

namespace saida {
using Texture = rhi::webgpu::Texture;
} // namespace saida

#else

#include "graphics/VmaFwd.hpp"
#include "rhi/Format.hpp"
#include "rhi/Sampler.hpp"

#include <cstdint>
#include <string>

namespace saida::rhi::vulkan {
class CommandEncoder;
}

namespace saida {

class VulkanDevice;
class Buffer;

// Loads an image file (via stb_image) into a sampled, device-local texture:
// owns the VkImage, its view and a sampler. RAII.
class Texture {
public:
    // `address` is the glTF sampler's wrap mode. Sprites whose UVs run outside
    // [0, 1] (the castle grounds' trees) need ClampToEdge: repeating tiles a
    // second copy of the art next to the first.
    Texture(VulkanDevice& device, const std::string& path, bool srgb = true,
            rhi::AddressMode address = rhi::AddressMode::Repeat);
    Texture(VulkanDevice& device, const uint8_t* pixels, uint32_t width, uint32_t height, rhi::Format format = rhi::Format::RGBA8Srgb, bool generateMipmaps = true,
            rhi::AddressMode address = rhi::AddressMode::Repeat);
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    VkImageView imageView() const { return imageView_; }
    VkSampler sampler() const { return sampler_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    // Updates the texture content dynamically (e.g. for UI)
    void updatePixels(const uint8_t* pixels, size_t size);
    
    // Updates the texture asynchronously during frame command recording
    void updatePixelsAsync(rhi::vulkan::CommandEncoder& encoder, Buffer& stagingBuffer,
                           uint32_t width, uint32_t height);
    
    void setBindlessIndex(uint32_t idx) { bindlessIndex_ = idx; }
    uint32_t bindlessIndex() const { return bindlessIndex_; }

    // GPU bytes used by the mip chain — part of the GPU resource accounting.
    uint64_t gpuBytes() const { return trackedBytes_; }

private:
    void createSampler();
    rhi::AddressMode address_ = rhi::AddressMode::Repeat;
    void generateMipmaps();
    VkImageView createImageView(VkFormat format, VkImageAspectFlags aspect);

    VulkanDevice& device_;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView imageView_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t mipLevels_ = 1;
    uint64_t trackedBytes_ = 0;
    std::string trackedCategory_;
    
    uint32_t bindlessIndex_ = ~0u;
};

} // namespace saida

#endif // SAIDA_RHI_WEBGPU
