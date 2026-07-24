#include "rhi/vulkan/Sampler.hpp"

#include "graphics/VulkanDevice.hpp"
#include "rhi/vulkan/Convert.hpp"

#include <stdexcept>

namespace saida::rhi::vulkan {

namespace {

VkFilter toVk(rhi::FilterMode mode) {
    return mode == rhi::FilterMode::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

VkSamplerMipmapMode toVkMipmap(rhi::FilterMode mode) {
    return mode == rhi::FilterMode::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                           : VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

VkSamplerAddressMode toVk(rhi::AddressMode mode) {
    switch (mode) {
        case rhi::AddressMode::Repeat:        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case rhi::AddressMode::ClampToEdge:   return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case rhi::AddressMode::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

} // namespace

Sampler::Sampler(VulkanDevice& device, const rhi::SamplerDesc& desc) : device_(device) {
    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = toVk(desc.magFilter);
    ci.minFilter = toVk(desc.minFilter);
    ci.mipmapMode = toVkMipmap(desc.mipFilter);
    ci.addressModeU = toVk(desc.addressMode);
    ci.addressModeV = toVk(desc.addressMode);
    ci.addressModeW = toVk(desc.addressMode);
    ci.compareEnable = desc.compareEnabled ? VK_TRUE : VK_FALSE;
    ci.compareOp = rhi::vulkan::toVk(desc.compare);
    ci.borderColor = desc.whiteBorder ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                      : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (vkCreateSampler(device_.device(), &ci, nullptr, &sampler_) != VK_SUCCESS)
        throw std::runtime_error("failed to create sampler");
}

Sampler::~Sampler() {
    vkDestroySampler(device_.device(), sampler_, nullptr);
}

} // namespace saida::rhi::vulkan
