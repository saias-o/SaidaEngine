#pragma once

#include <cstdint>

namespace saida::rhi {

enum class CompareOp : uint32_t {
    Never,
    Less,
    Equal,
    LessOrEqual,
    Greater,
    NotEqual,
    GreaterOrEqual,
    Always,
};

enum class CullMode : uint32_t {
    None,
    Front,
    Back,
};

enum class Topology : uint32_t {
    TriangleList,
    TriangleStrip,
    LineList,
    PointList,
};

// Moved here from graphics/Pipeline.hpp (aliased back into saida:: there) so the
// WebGPU backend's pipeline desc can share it.
enum class BlendMode : uint32_t {
    None,
    Alpha,
    Additive,
};

} // namespace saida::rhi
