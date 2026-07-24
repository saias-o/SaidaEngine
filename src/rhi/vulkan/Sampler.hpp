#pragma once

#include "rhi/Sampler.hpp"

#include <vulkan/vulkan.h>

namespace saida {
class VulkanDevice;
}

namespace saida::rhi::vulkan {

class Sampler {
public:
    Sampler(VulkanDevice& device, const rhi::SamplerDesc& desc);
    ~Sampler();
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    VkSampler handle() const { return sampler_; }

private:
    VulkanDevice& device_;
    VkSampler sampler_ = VK_NULL_HANDLE;
};

} // namespace saida::rhi::vulkan
