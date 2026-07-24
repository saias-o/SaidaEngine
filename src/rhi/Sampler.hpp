#pragma once

#include "rhi/PipelineState.hpp"  // CompareOp

#include <cstdint>

namespace saida::rhi {

enum class FilterMode : uint32_t { Nearest, Linear };

enum class AddressMode : uint32_t { Repeat, ClampToEdge, ClampToBorder };

struct SamplerDesc {
    FilterMode magFilter = FilterMode::Linear;
    FilterMode minFilter = FilterMode::Linear;
    FilterMode mipFilter = FilterMode::Nearest;
    AddressMode addressMode = AddressMode::ClampToEdge;  // all three axes
    bool compareEnabled = false;         // hardware PCF (shadow maps)
    CompareOp compare = CompareOp::Less;
    bool whiteBorder = false;            // ClampToBorder: white = "outside frustum is lit"
};

} // namespace saida::rhi
