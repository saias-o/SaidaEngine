#pragma once

#include <cstdint>

// Transitions stay explicit so each backend can own its synchronization model.

namespace saida::rhi {

enum class ResourceState : uint32_t {
    Undefined,         // contents discarded (valid as source state only)
    ShaderRead,        // sampled/read in any shader stage
    ColorAttachment,   // render target color write
    DepthWrite,        // depth attachment write
    DepthRead,         // depth sampled in shaders (shadow maps, AO depth)
    StorageReadWrite,  // storage image/buffer access (compute or fragment)
    CopySrc,
    CopyDst,
    Present,           // ready for presentation
};

enum class LoadOp : uint32_t {
    Load,
    Clear,
    DontCare,
};

enum class IndexType : uint32_t {
    Uint16,
    Uint32,
};

// Auto cannot infer depth when an image moves from Undefined to ShaderRead.
enum class TextureAspect : uint32_t {
    Auto,
    Color,
    Depth,
};

} // namespace saida::rhi
