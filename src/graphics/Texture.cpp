#include "graphics/Texture.hpp"

#include "core/Log.hpp"
#include "core/Profiler.hpp"
#include "graphics/Buffer.hpp"
#include "graphics/MemoryProfiler.hpp"
#include "graphics/VulkanDevice.hpp"
#include "rhi/vulkan/CommandEncoder.hpp"
#include "rhi/vulkan/Format.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include "stb_image.h"
#include "vk_mem_alloc.h"

#include <stdexcept>

namespace {
// A missing texture file must never crash the app (a stale/renamed asset path
// should degrade, not abort). Returns a freshly malloc'd 2x2 magenta/charcoal
// checker — the classic "missing texture" placeholder — freeable via
// stbi_image_free (which is just free()).
void* magentaPlaceholder() {
    auto* px = static_cast<unsigned char*>(std::malloc(2 * 2 * 4));
    const unsigned char m[4] = {255, 0, 255, 255};
    const unsigned char d[4] = {24, 24, 24, 255};
    std::memcpy(px + 0, m, 4);  std::memcpy(px + 4, d, 4);
    std::memcpy(px + 8, d, 4);  std::memcpy(px + 12, m, 4);
    return px;
}
} // namespace

namespace saida {

namespace {

uint64_t mipChainBytes(uint32_t width, uint32_t height, uint32_t bytesPerPixel, uint32_t levels) {
    uint64_t total = 0;
    uint32_t w = width;
    uint32_t h = height;
    for (uint32_t i = 0; i < levels; ++i) {
        total += static_cast<uint64_t>(std::max(1u, w)) * std::max(1u, h) * bytesPerPixel;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    return total;
}

} // namespace

Texture::Texture(VulkanDevice& device, const std::string& path, bool srgb) : device_(device) {
    SAIDA_PROFILE_SCOPE("Resource/LoadTextureFile");
    int texWidth, texHeight, texChannels;
    bool isHdr = stbi_is_hdr(path.c_str());
    VkFormat format;
    VkDeviceSize imageSize;
    void* pixels = nullptr;
    
    if (isHdr) {
        format = VK_FORMAT_R32G32B32A32_SFLOAT;
        pixels = stbi_loadf(path.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
    } else {
        format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        pixels = stbi_load(path.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
    }
    
    if (!pixels) {
        Log::warn("failed to load texture '", path, "' — using magenta placeholder");
        isHdr = false;
        format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        texWidth = texHeight = 2;
        texChannels = 4;
        pixels = magentaPlaceholder();
    }

    width_ = static_cast<uint32_t>(texWidth);
    height_ = static_cast<uint32_t>(texHeight);
    mipLevels_ = static_cast<uint32_t>(std::floor(std::log2(std::max(width_, height_)))) + 1;

    imageSize = static_cast<VkDeviceSize>(width_) * height_ * (isHdr ? 16 : 4);

    Buffer staging(device_, imageSize, rhi::BufferUsage::TransferSrc, MemoryUsage::HostVisible);
    staging.write(pixels, imageSize);
    stbi_image_free(pixels);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width_;
    imageInfo.extent.height = height_;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels_;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateImage(device_.allocator(), &imageInfo, &allocInfo,
                       &image_, &allocation_, nullptr) != VK_SUCCESS)
        throw std::runtime_error("failed to create texture image");
    trackedBytes_ = mipChainBytes(width_, height_, isHdr ? 16u : 4u, mipLevels_);
    trackedCategory_ = isHdr ? "Texture/HDR" : "Texture/2D";
    MemoryProfiler::registerAllocation(trackedCategory_, trackedBytes_);

    device_.withSingleTimeEncoder([&](rhi::vulkan::CommandEncoder& enc) {
        enc.transition(image_, rhi::ResourceState::Undefined, rhi::ResourceState::CopyDst);
        enc.copyBufferToTexture(staging, image_, width_, height_);
    });

    generateMipmaps();

    imageView_ = createImageView(format, VK_IMAGE_ASPECT_COLOR_BIT);
    createSampler();
}

Texture::Texture(VulkanDevice& device, const uint8_t* pixels, uint32_t width, uint32_t height, rhi::Format fmt, bool genMipmaps)
    : device_(device), width_(width), height_(height) {
    SAIDA_PROFILE_SCOPE("Resource/CreateMemoryTexture");
    const VkFormat format = rhi::vulkan::toVk(fmt);
    const uint32_t texelBytes = rhi::bytesPerTexel(fmt);
    mipLevels_ = 1;
    if (genMipmaps && (width > 1 || height > 1)) {
        mipLevels_ = static_cast<uint32_t>(std::floor(std::log2(std::max(width_, height_)))) + 1;
    }

    VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * texelBytes;

    Buffer staging(device_, imageSize, rhi::BufferUsage::TransferSrc, MemoryUsage::HostVisible);
    staging.write(pixels, imageSize);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width_;
    imageInfo.extent.height = height_;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels_;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateImage(device_.allocator(), &imageInfo, &allocInfo,
                       &image_, &allocation_, nullptr) != VK_SUCCESS)
        throw std::runtime_error("failed to create texture image");
    trackedBytes_ = mipChainBytes(width_, height_, texelBytes, mipLevels_);
    trackedCategory_ = "Texture/Dynamic";
    MemoryProfiler::registerAllocation(trackedCategory_, trackedBytes_);

    device_.withSingleTimeEncoder([&](rhi::vulkan::CommandEncoder& enc) {
        enc.transition(image_, rhi::ResourceState::Undefined, rhi::ResourceState::CopyDst);
        enc.copyBufferToTexture(staging, image_, width_, height_);
        if (mipLevels_ == 1)
            enc.transition(image_, rhi::ResourceState::CopyDst, rhi::ResourceState::ShaderRead);
    });

    if (mipLevels_ > 1) generateMipmaps();

    imageView_ = createImageView(format, VK_IMAGE_ASPECT_COLOR_BIT);
    createSampler();
}

Texture::~Texture() {
    MemoryProfiler::unregisterAllocation(trackedCategory_, trackedBytes_);
    vkDestroySampler(device_.device(), sampler_, nullptr);
    vkDestroyImageView(device_.device(), imageView_, nullptr);
    vmaDestroyImage(device_.allocator(), image_, allocation_);
}

void Texture::createSampler() {
    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = VK_FILTER_LINEAR;
    ci.minFilter = VK_FILTER_LINEAR;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    float maxAnisotropy = device_.maxAnisotropy();
    ci.anisotropyEnable = maxAnisotropy > 0.0f ? VK_TRUE : VK_FALSE;
    ci.maxAnisotropy = std::min(maxAnisotropy, 8.0f);
    ci.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    ci.unnormalizedCoordinates = VK_FALSE;
    ci.compareEnable = VK_FALSE;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.minLod = 0.0f;
    ci.maxLod = static_cast<float>(mipLevels_);
    ci.mipLodBias = 0.0f;

    if (vkCreateSampler(device_.device(), &ci, nullptr, &sampler_) != VK_SUCCESS)
        throw std::runtime_error("failed to create texture sampler");
}

VkImageView Texture::createImageView(VkFormat format, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = image_;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = format;
    ci.subresourceRange.aspectMask = aspect;
    ci.subresourceRange.baseMipLevel = 0;
    ci.subresourceRange.levelCount = mipLevels_;
    ci.subresourceRange.baseArrayLayer = 0;
    ci.subresourceRange.layerCount = 1;
    VkImageView view;
    if (vkCreateImageView(device_.device(), &ci, nullptr, &view) != VK_SUCCESS)
        throw std::runtime_error("failed to create image view");
    return view;
}

// Mip generation stays backend-local because the APIs expose different mechanisms.
void Texture::generateMipmaps() {
    VkCommandBuffer cmd = device_.beginSingleTimeCommands();

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = image_;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.subresourceRange.levelCount = 1;

    int32_t mipWidth = width_;
    int32_t mipHeight = height_;

    for (uint32_t i = 1; i < mipLevels_; i++) {
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, nullptr, 0, nullptr, 1, &barrier);

        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = { mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1 };
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;

        vkCmdBlitImage(cmd,
            image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit,
            VK_FILTER_LINEAR);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
            0, nullptr, 0, nullptr, 1, &barrier);

        if (mipWidth > 1) mipWidth /= 2;
        if (mipHeight > 1) mipHeight /= 2;
    }

    barrier.subresourceRange.baseMipLevel = mipLevels_ - 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
        0, nullptr, 0, nullptr, 1, &barrier);

    device_.endSingleTimeCommands(cmd);
}

void Texture::updatePixels(const uint8_t* pixels, size_t size) {
    SAIDA_PROFILE_SCOPE("Texture/UpdatePixels");
    if (size != static_cast<size_t>(width_ * height_ * 4)) {
        throw std::runtime_error("updatePixels size mismatch");
    }

    Buffer staging(device_, size, rhi::BufferUsage::TransferSrc, MemoryUsage::HostVisible);
    staging.write(pixels, size);

    device_.withSingleTimeEncoder([&](rhi::vulkan::CommandEncoder& enc) {
        enc.transition(image_, rhi::ResourceState::ShaderRead, rhi::ResourceState::CopyDst);
        enc.copyBufferToTexture(staging, image_, width_, height_);
        if (mipLevels_ == 1)
            enc.transition(image_, rhi::ResourceState::CopyDst, rhi::ResourceState::ShaderRead);
    });

    if (mipLevels_ > 1) generateMipmaps();
}

void Texture::updatePixelsAsync(rhi::vulkan::CommandEncoder& encoder, Buffer& stagingBuffer,
                                uint32_t width, uint32_t height) {
    SAIDA_PROFILE_SCOPE("Texture/UpdatePixelsAsync");
    if (width != width_ || height != height_) {
        throw std::runtime_error("updatePixelsAsync size mismatch");
    }

    // UI textures are single-mip; regenerating mips mid-frame would be wrong anyway.
    encoder.transition(image_, rhi::ResourceState::ShaderRead, rhi::ResourceState::CopyDst);
    encoder.copyBufferToTexture(stagingBuffer, image_, width, height);
    encoder.transition(image_, rhi::ResourceState::CopyDst, rhi::ResourceState::ShaderRead);
}

} // namespace saida
