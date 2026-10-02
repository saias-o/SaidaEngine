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

// The scene's depth is reversed: 1 at the near plane, 0 at the far plane.
// A float depth buffer holds its precision near 0, and perspective puts the
// distance there: reversed, a D32 buffer resolves about a centimetre at 100 km
// where the conventional one could not tell two ridges kilometres apart (SPEC
// "Reversed depth"). Shadow maps keep their own conventional depth.
inline constexpr float kDepthFar = 0.0f;  // the clear value, and nothing drawn
inline constexpr CompareOp kDepthCloser = CompareOp::Greater;
inline constexpr CompareOp kDepthCloserOrEqual = CompareOp::GreaterOrEqual;

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
