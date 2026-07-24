#pragma once

#include <cstdint>

namespace saida::rhi {

enum class ShaderStages : uint32_t {
    None = 0,
    Vertex = 1,
    Fragment = 2,
    Compute = 4,
    VertexFragment = 3,
    All = 7,
};

inline constexpr ShaderStages operator|(ShaderStages a, ShaderStages b) {
    return static_cast<ShaderStages>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline constexpr bool hasStage(ShaderStages mask, ShaderStages stage) {
    return (static_cast<uint32_t>(mask) & static_cast<uint32_t>(stage)) != 0;
}

} // namespace saida::rhi
