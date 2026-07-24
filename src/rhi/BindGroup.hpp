#pragma once

#include "rhi/ShaderStages.hpp"

#include <cstdint>

namespace saida::rhi {

enum class BindingType : uint32_t {
    UniformBuffer,
    StorageBuffer,
    CombinedImageSampler,  // desktop GLSL; web shaders split texture/sampler (16.2)
    SampledTexture,
    Sampler,
    StorageImage,
};

struct BindGroupLayoutEntry {
    uint32_t binding = 0;
    BindingType type = BindingType::UniformBuffer;
    ShaderStages visibility = ShaderStages::VertexFragment;
    uint32_t count = 1;  // array size (e.g. bindless texture arrays)
};

} // namespace saida::rhi
