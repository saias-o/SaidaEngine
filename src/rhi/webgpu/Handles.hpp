#pragma once

#include "rhi/webgpu/WebGpu.hpp"

#include <cstdint>


namespace saida::rhi::webgpu {

using TextureHandle = WGPUTexture;
using TextureView = WGPUTextureView;
using SamplerHandle = WGPUSampler;

struct Extent2D {
    uint32_t width = 0;
    uint32_t height = 0;
};

struct Offset2D {
    int32_t x = 0;
    int32_t y = 0;
};

struct Rect2D {
    Offset2D offset{};
    Extent2D extent{};
};

using SampleCount = uint32_t;

} // namespace saida::rhi::webgpu
