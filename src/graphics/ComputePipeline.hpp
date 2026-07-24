#pragma once

#ifdef SAIDA_RHI_WEBGPU

#include "rhi/webgpu/Pipeline.hpp"

namespace saida {
using ComputePipeline = rhi::webgpu::ComputePipeline;
} // namespace saida

#else

#include "rhi/vulkan/BindGroup.hpp"  // BindGroupLayoutRef

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

namespace saida {

class VulkanDevice;

// A compute pipeline built from a single .comp SPIR-V module. The reusable brick
// for every GPU compute pass to come (Radiance Cascades merge, radiance-cache
// updates, froxel scatter, tonemapping…). Mirrors the RAII style of Pipeline.
class ComputePipeline {
public:
    // pushConstantSize in bytes (0 = none); push constants are visible to the
    // compute stage. Layouts are rhi bind group layouts (raw form = bindless
    // interop only, see BindGroupLayoutRef).
    ComputePipeline(VulkanDevice& device, const std::string& compPath,
                    const std::vector<rhi::vulkan::BindGroupLayoutRef>& setLayouts,
                    uint32_t pushConstantSize = 0);
    ~ComputePipeline();
    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    void bind(VkCommandBuffer cmd) const;
    VkPipelineLayout layout() const { return layout_; }

    // Ceil-divide helper: groups needed to cover `total` items at `localSize`.
    static uint32_t groupCount(uint32_t total, uint32_t localSize) {
        return (total + localSize - 1) / localSize;
    }

private:
    VulkanDevice& device_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

} // namespace saida

#endif // SAIDA_RHI_WEBGPU
