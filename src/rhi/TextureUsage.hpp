#pragma once

#include <cstdint>

namespace saida::rhi {

enum class TextureUsage : uint32_t {
    None = 0,
    Sampled = 1,          // read in shaders
    Storage = 2,          // imageStore/imageLoad (compute GI atlases, voxel grid)
    ColorAttachment = 4,  // rendered into
    DepthAttachment = 8,
    CopySrc = 16,
    CopyDst = 32,         // cleared / staging-uploaded
    Transient = 64,       // never stored (MSAA scene target) — lazy allocation
};

inline constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) {
    return static_cast<TextureUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline constexpr bool hasUsage(TextureUsage mask, TextureUsage usage) {
    return (static_cast<uint32_t>(mask) & static_cast<uint32_t>(usage)) != 0;
}

} // namespace saida::rhi
