#pragma once

#include <vulkan/vulkan.h>


namespace saida::rhi::vulkan {

using TextureHandle = VkImage;
using TextureView = VkImageView;
using SamplerHandle = VkSampler;
using Extent2D = VkExtent2D;
using Rect2D = VkRect2D;
using SampleCount = VkSampleCountFlagBits;

} // namespace saida::rhi::vulkan
