#include "graphics/ComputePipeline.hpp"

#include "graphics/VulkanDevice.hpp"

#include <fstream>
#include <stdexcept>

namespace saida {

namespace {

std::vector<char> readFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open())
        throw std::runtime_error("failed to open file: " + filename);
    size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), size);
    return buffer;
}

} // namespace

ComputePipeline::ComputePipeline(VulkanDevice& device, const std::string& compPath,
                                 const std::vector<rhi::vulkan::BindGroupLayoutRef>& layoutRefs,
                                 uint32_t pushConstantSize)
    : device_(device) {
    std::vector<VkDescriptorSetLayout> setLayouts;
    setLayouts.reserve(layoutRefs.size());
    for (const rhi::vulkan::BindGroupLayoutRef& ref : layoutRefs)
        setLayouts.push_back(ref.handle());

    auto code = readFile(compPath);
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = code.size();
    smci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module;
    if (vkCreateShaderModule(device_.device(), &smci, nullptr, &module) != VK_SUCCESS)
        throw std::runtime_error("failed to create compute shader module");

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = pushConstantSize;

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutCI.pSetLayouts = setLayouts.empty() ? nullptr : setLayouts.data();
    layoutCI.pushConstantRangeCount = pushConstantSize > 0 ? 1 : 0;
    layoutCI.pPushConstantRanges = pushConstantSize > 0 ? &pushRange : nullptr;
    if (vkCreatePipelineLayout(device_.device(), &layoutCI, nullptr, &layout_) != VK_SUCCESS)
        throw std::runtime_error("failed to create compute pipeline layout");

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";

    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = stage;
    ci.layout = layout_;
    if (vkCreateComputePipelines(device_.device(), device_.pipelineCache(), 1, &ci, nullptr,
        &pipeline_) != VK_SUCCESS)
        throw std::runtime_error("failed to create compute pipeline");

    vkDestroyShaderModule(device_.device(), module, nullptr);
}

ComputePipeline::~ComputePipeline() {
    vkDestroyPipeline(device_.device(), pipeline_, nullptr);
    vkDestroyPipelineLayout(device_.device(), layout_, nullptr);
}

void ComputePipeline::bind(VkCommandBuffer cmd) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
}

} // namespace saida
