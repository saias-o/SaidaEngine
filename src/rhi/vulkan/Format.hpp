#pragma once

#include "rhi/Format.hpp"

#include <vulkan/vulkan.h>

#include <stdexcept>

namespace saida::rhi::vulkan {

inline VkFormat toVk(Format f) {
    switch (f) {
        case Format::Undefined:            return VK_FORMAT_UNDEFINED;
        case Format::RGBA8Unorm:           return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::RGBA8Srgb:            return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::BGRA8Unorm:           return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::BGRA8Srgb:            return VK_FORMAT_B8G8R8A8_SRGB;
        case Format::RG16Float:            return VK_FORMAT_R16G16_SFLOAT;
        case Format::RGBA16Float:          return VK_FORMAT_R16G16B16A16_SFLOAT;
        case Format::RG32Float:            return VK_FORMAT_R32G32_SFLOAT;
        case Format::RGB32Float:           return VK_FORMAT_R32G32B32_SFLOAT;
        case Format::RGBA32Float:          return VK_FORMAT_R32G32B32A32_SFLOAT;
        case Format::RGBA32Sint:           return VK_FORMAT_R32G32B32A32_SINT;
        case Format::Depth16:              return VK_FORMAT_D16_UNORM;
        case Format::Depth32Float:         return VK_FORMAT_D32_SFLOAT;
        case Format::Depth24Stencil8:      return VK_FORMAT_D24_UNORM_S8_UINT;
        case Format::Depth32FloatStencil8: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

// Runtime surface and device queries expose VkFormat rather than the neutral type.
inline Format fromVk(VkFormat f) {
    switch (f) {
        case VK_FORMAT_UNDEFINED:            return Format::Undefined;
        case VK_FORMAT_R8G8B8A8_UNORM:       return Format::RGBA8Unorm;
        case VK_FORMAT_R8G8B8A8_SRGB:        return Format::RGBA8Srgb;
        case VK_FORMAT_B8G8R8A8_UNORM:       return Format::BGRA8Unorm;
        case VK_FORMAT_B8G8R8A8_SRGB:        return Format::BGRA8Srgb;
        case VK_FORMAT_R16G16_SFLOAT:        return Format::RG16Float;
        case VK_FORMAT_R16G16B16A16_SFLOAT:  return Format::RGBA16Float;
        case VK_FORMAT_R32G32_SFLOAT:        return Format::RG32Float;
        case VK_FORMAT_R32G32B32_SFLOAT:     return Format::RGB32Float;
        case VK_FORMAT_R32G32B32A32_SFLOAT:  return Format::RGBA32Float;
        case VK_FORMAT_R32G32B32A32_SINT:    return Format::RGBA32Sint;
        case VK_FORMAT_D16_UNORM:            return Format::Depth16;
        case VK_FORMAT_D32_SFLOAT:           return Format::Depth32Float;
        case VK_FORMAT_D24_UNORM_S8_UINT:    return Format::Depth24Stencil8;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:   return Format::Depth32FloatStencil8;
        default: throw std::runtime_error("rhi::vulkan::fromVk: unmapped VkFormat");
    }
}

} // namespace saida::rhi::vulkan
