#include "graphics/Pipeline.hpp"

#include "graphics/Mesh.hpp"
#include "graphics/VulkanDevice.hpp"
#include "rhi/vulkan/BindGroup.hpp"
#include "rhi/vulkan/Convert.hpp"
#include "rhi/vulkan/Format.hpp"

#include <fstream>
#include <stdexcept>
#include <vector>

namespace saida {

namespace {

std::vector<char> readFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open())
        throw std::runtime_error("failed to open file: " + filename);
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    return buffer;
}

} // namespace

Pipeline::Pipeline(VulkanDevice& device, const Desc& desc) : device_(device) {
    BuildInfo info;
    info.vertPath = desc.vertPath;
    info.fragPath = desc.fragPath;
    info.colorFormats.reserve(desc.colorFormats.size());
    for (rhi::Format format : desc.colorFormats)
        info.colorFormats.push_back(rhi::vulkan::toVk(format));
    info.depthFormat = rhi::vulkan::toVk(desc.depthFormat);
    info.setLayouts.reserve(desc.bindGroupLayouts.size());
    for (const rhi::vulkan::BindGroupLayoutRef& layout : desc.bindGroupLayouts)
        info.setLayouts.push_back(layout.handle());
    info.samples = static_cast<VkSampleCountFlagBits>(desc.samples);
    info.useVertexInput = desc.vertexInput;
    info.useDepth = desc.depthTest;
    info.pushConstantSize = desc.pushConstantSize;
    info.depthWrite = desc.depthWrite;
    info.depthCompare = rhi::vulkan::toVk(desc.depthCompare);
    info.cullMode = rhi::vulkan::toVk(desc.cullMode);
    info.blendMode = desc.blendMode;
    info.topology = rhi::vulkan::toVk(desc.topology);
    info.viewMask = desc.viewMask;
    info.pushStages = rhi::vulkan::toVk(desc.pushConstantStages);
    info.depthBias = desc.depthBias;
    info.depthBiasConstant = desc.depthBiasConstant;
    info.depthBiasSlope = desc.depthBiasSlope;
    build(info);
}

void Pipeline::build(const BuildInfo& info) {
    pushStages_ = info.pushStages;

    auto vertCode = readFile(info.vertPath);
    VkShaderModule vertModule = createShaderModule(vertCode);
    VkShaderModule fragModule = VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    uint32_t stageCount = 1;

    if (!info.fragPath.empty()) {
        auto fragCode = readFile(info.fragPath);
        fragModule = createShaderModule(fragCode);
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragModule;
        stages[1].pName = "main";
        stageCount = 2;
    }

    auto bindingDesc = Vertex::bindingDescription();
    auto attrDescs = Vertex::attributeDescriptions();

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (info.useVertexInput) {
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &bindingDesc;
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrDescs.size());
        vertexInput.pVertexAttributeDescriptions = attrDescs.data();
    }

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = info.topology;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = info.cullMode;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = info.depthBias ? VK_TRUE : VK_FALSE;
    rasterizer.depthBiasConstantFactor = info.depthBiasConstant;
    rasterizer.depthBiasSlopeFactor = info.depthBiasSlope;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = info.samples;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = info.useDepth ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = (info.useDepth && info.depthWrite) ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = info.depthCompare;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttach{};
    colorBlendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                    | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttach.blendEnable = info.blendMode == BlendMode::None ? VK_FALSE : VK_TRUE;
    if (info.blendMode == BlendMode::Alpha) {
        colorBlendAttach.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorBlendAttach.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttach.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttach.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttach.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (info.blendMode == BlendMode::Additive) {
        colorBlendAttach.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorBlendAttach.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttach.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttach.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttach.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    // One blend state per color attachment (0 for depth-only pipelines).
    std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(
        info.colorFormats.size(), colorBlendAttach);

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.logicOpEnable = VK_FALSE;
    colorBlend.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
    colorBlend.pAttachments = blendAttachments.empty() ? nullptr : blendAttachments.data();

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // Per-object push constants: model matrix (vertex) + params (fragment).
    // Matches PushConstants in shader.vert / shader.frag (mat4 + vec4 = 80 bytes).
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = info.pushStages;
    pushRange.offset = 0;
    pushRange.size = info.pushConstantSize > 0 ? info.pushConstantSize
                                               : (sizeof(glm::mat4) + sizeof(glm::vec4));

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount = static_cast<uint32_t>(info.setLayouts.size());
    layoutCI.pSetLayouts = info.setLayouts.data();
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(device_.device(), &layoutCI, nullptr, &layout_) != VK_SUCCESS)
        throw std::runtime_error("failed to create pipeline layout");

    // Dynamic rendering: declare the attachment formats instead of a render pass.
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = static_cast<uint32_t>(info.colorFormats.size());
    renderingInfo.pColorAttachmentFormats = info.colorFormats.empty() ? nullptr : info.colorFormats.data();
    renderingInfo.depthAttachmentFormat = info.depthFormat;
    // Multiview (XR stereo): viewMask != 0 makes one draw render to all set views
    // (gl_ViewIndex selects per-eye matrices). 0 = ordinary single-view rendering.
    renderingInfo.viewMask = info.viewMask;

    VkGraphicsPipelineCreateInfo pipelineCI{};
    pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCI.pNext = &renderingInfo;
    pipelineCI.stageCount = stageCount;
    pipelineCI.pStages = stages;
    pipelineCI.pVertexInputState = &vertexInput;
    pipelineCI.pInputAssemblyState = &inputAssembly;
    pipelineCI.pViewportState = &viewportState;
    pipelineCI.pRasterizationState = &rasterizer;
    pipelineCI.pMultisampleState = &multisampling;
    pipelineCI.pDepthStencilState = &depthStencil;
    pipelineCI.pColorBlendState = &colorBlend;
    pipelineCI.pDynamicState = &dynamicState;
    pipelineCI.layout = layout_;
    pipelineCI.renderPass = VK_NULL_HANDLE;
    pipelineCI.subpass = 0;

    if (vkCreateGraphicsPipelines(device_.device(), device_.pipelineCache(), 1, &pipelineCI, nullptr,
        &pipeline_) != VK_SUCCESS)
        throw std::runtime_error("failed to create graphics pipeline");

    if (fragModule) vkDestroyShaderModule(device_.device(), fragModule, nullptr);
    vkDestroyShaderModule(device_.device(), vertModule, nullptr);
}

Pipeline::~Pipeline() {
    vkDestroyPipeline(device_.device(), pipeline_, nullptr);
    vkDestroyPipelineLayout(device_.device(), layout_, nullptr);
}

VkShaderModule Pipeline::createShaderModule(const std::vector<char>& code) const {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod;
    if (vkCreateShaderModule(device_.device(), &ci, nullptr, &mod) != VK_SUCCESS)
        throw std::runtime_error("failed to create shader module");
    return mod;
}

void Pipeline::bind(VkCommandBuffer cmd) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
}

} // namespace saida
