#pragma once

#ifdef SAIDA_RHI_WEBGPU

#include "rhi/webgpu/Pipeline.hpp"

namespace saida {
using BlendMode = rhi::BlendMode;
using Pipeline = rhi::webgpu::Pipeline;
} // namespace saida

#else

#include "rhi/Format.hpp"
#include "rhi/PipelineState.hpp"
#include "rhi/ShaderStages.hpp"

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

#include "rhi/vulkan/BindGroup.hpp"  // BindGroupLayoutRef

namespace saida {

class VulkanDevice;

using BlendMode = rhi::BlendMode;

// A graphics pipeline configured for the Vertex layout, built against dynamic
// rendering attachment formats (no VkRenderPass) and descriptor set layouts.
class Pipeline {
public:
    struct Desc {
        std::string vertPath;
        std::string fragPath;
        std::vector<rhi::Format> colorFormats;
        rhi::Format depthFormat = rhi::Format::Undefined;
        std::vector<rhi::vulkan::BindGroupLayoutRef> bindGroupLayouts;
        uint32_t samples = 1;
        bool vertexInput = true;
        bool depthTest = true;
        bool depthWrite = true;
        rhi::CompareOp depthCompare = rhi::CompareOp::Less;
        rhi::CullMode cullMode = rhi::CullMode::Back;
        rhi::BlendMode blendMode = rhi::BlendMode::None;
        rhi::Topology topology = rhi::Topology::TriangleList;
        uint32_t pushConstantSize = 0;
        rhi::ShaderStages pushConstantStages = rhi::ShaderStages::VertexFragment;
        uint32_t viewMask = 0;
        bool depthBias = false;
        float depthBiasConstant = 0.0f;
        float depthBiasSlope = 0.0f;
    };

    Pipeline(VulkanDevice& device, const Desc& desc);
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void bind(VkCommandBuffer cmd) const;
    VkPipelineLayout layout() const { return layout_; }
    VkShaderStageFlags pushConstantStages() const { return pushStages_; }

private:
    struct BuildInfo {
        std::string vertPath;
        std::string fragPath;
        std::vector<VkFormat> colorFormats;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        std::vector<VkDescriptorSetLayout> setLayouts;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        bool useVertexInput = true;
        bool useDepth = true;
        uint32_t pushConstantSize = 0;
        bool depthWrite = true;
        VkCompareOp depthCompare = VK_COMPARE_OP_LESS;
        VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
        BlendMode blendMode = BlendMode::None;
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        uint32_t viewMask = 0;
        VkShaderStageFlags pushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bool depthBias = false;
        float depthBiasConstant = 0.0f;
        float depthBiasSlope = 0.0f;
    };

    void build(const BuildInfo& info);
    VkShaderModule createShaderModule(const std::vector<char>& code) const;

    VulkanDevice& device_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkShaderStageFlags pushStages_ = 0;
};

} // namespace saida

#endif // SAIDA_RHI_WEBGPU
