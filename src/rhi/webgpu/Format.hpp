#pragma once

#include "rhi/Format.hpp"
#include "rhi/webgpu/WebGpu.hpp"


namespace saida::rhi::webgpu {

inline WGPUTextureFormat toWgpu(Format f) {
    switch (f) {
        case Format::Undefined:            return WGPUTextureFormat_Undefined;
        case Format::RGBA8Unorm:           return WGPUTextureFormat_RGBA8Unorm;
        case Format::RGBA8Srgb:            return WGPUTextureFormat_RGBA8UnormSrgb;
        case Format::BGRA8Unorm:           return WGPUTextureFormat_BGRA8Unorm;
        case Format::BGRA8Srgb:            return WGPUTextureFormat_BGRA8UnormSrgb;
        case Format::RG16Float:            return WGPUTextureFormat_RG16Float;
        case Format::RGBA16Float:          return WGPUTextureFormat_RGBA16Float;
        case Format::RG32Float:            return WGPUTextureFormat_RG32Float;
        case Format::RGB32Float:           return WGPUTextureFormat_Undefined;  // not a texture format on web
        case Format::RGBA32Float:          return WGPUTextureFormat_RGBA32Float;
        case Format::RGBA32Sint:           return WGPUTextureFormat_RGBA32Sint;
        case Format::Depth16:              return WGPUTextureFormat_Depth16Unorm;
        case Format::Depth32Float:         return WGPUTextureFormat_Depth32Float;
        case Format::Depth24Stencil8:      return WGPUTextureFormat_Depth24PlusStencil8;
        case Format::Depth32FloatStencil8: return WGPUTextureFormat_Depth32FloatStencil8;
    }
    return WGPUTextureFormat_Undefined;
}

} // namespace saida::rhi::webgpu
