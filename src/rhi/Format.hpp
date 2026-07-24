#pragma once

#include <cstdint>

namespace saida::rhi {

enum class Format : uint32_t {
    Undefined = 0,

    // 8-bit unsigned normalized / sRGB
    RGBA8Unorm,
    RGBA8Srgb,
    BGRA8Unorm,
    BGRA8Srgb,

    // floating-point colour
    RG16Float,
    RGBA16Float,
    RG32Float,
    RGB32Float,
    RGBA32Float,
    RGBA32Sint,

    // depth / stencil
    Depth16,
    Depth32Float,
    Depth24Stencil8,
    Depth32FloatStencil8,
};

inline constexpr bool isDepthFormat(Format f) {
    return f == Format::Depth16 || f == Format::Depth32Float ||
           f == Format::Depth24Stencil8 || f == Format::Depth32FloatStencil8;
}

// Approximate bytes per texel — memory-profiler accounting only.
inline constexpr uint32_t bytesPerTexel(Format f) {
    switch (f) {
        case Format::Undefined:            return 0;
        case Format::RGBA8Unorm:
        case Format::RGBA8Srgb:
        case Format::BGRA8Unorm:
        case Format::BGRA8Srgb:            return 4;
        case Format::RG16Float:            return 4;
        case Format::RGBA16Float:          return 8;
        case Format::RG32Float:            return 8;
        case Format::RGB32Float:           return 12;
        case Format::RGBA32Float:          return 16;
        case Format::RGBA32Sint:           return 16;
        case Format::Depth16:              return 2;
        case Format::Depth32Float:         return 4;
        case Format::Depth24Stencil8:      return 4;
        case Format::Depth32FloatStencil8: return 5;
    }
    return 0;
}

} // namespace saida::rhi
