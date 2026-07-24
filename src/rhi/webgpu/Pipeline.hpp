#pragma once

#include "rhi/Format.hpp"
#include "rhi/PipelineState.hpp"
#include "rhi/ShaderStages.hpp"
#include "rhi/webgpu/WebGpu.hpp"

#include <memory>
#include <string>
#include <vector>

// Push constants use a dynamic-offset uniform slice at group 3 on WebGPU.

namespace saida::rhi::webgpu {

class BindGroupLayout;
class Device;

class Pipeline {
public:
    struct Desc {
        std::string vertPath;
        std::string fragPath;
        std::vector<rhi::Format> colorFormats;
        rhi::Format depthFormat = rhi::Format::Undefined;
        std::vector<const BindGroupLayout*> bindGroupLayouts;
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
        // writeMask None on every color target: for passes whose fragment writes
        // nothing (voxelize's dummy attachment — WebGPU forbids attachment-less
        // passes, so a throwaway target keeps the rasterizer running).
        bool colorWrite = true;
        bool depthBias = false;
        float depthBiasConstant = 0.0f;
        float depthBiasSlope = 0.0f;
    };

    Pipeline(Device& device, const Desc& desc);
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    WGPURenderPipeline handle() const { return pipeline_; }
    uint32_t pushConstantSize() const { return pushSize_; }
    WGPUBindGroup pushBindGroup() const { return pushGroup_; }  // group 3 slice
    uint32_t groupCountUsed() const { return groupCount_; }     // incl. gaps

private:
    Device& device_;
    WGPURenderPipeline pipeline_ = nullptr;
    WGPUBindGroup pushGroup_ = nullptr;
    WGPUBindGroupLayout pushLayout_ = nullptr;
    uint32_t pushSize_ = 0;
    uint32_t groupCount_ = 0;
};

class ComputePipeline {
public:
    ComputePipeline(Device& device, const std::string& compPath,
                    const std::vector<const BindGroupLayout*>& setLayouts,
                    uint32_t pushConstantSize = 0);
    ~ComputePipeline();
    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    WGPUComputePipeline handle() const { return pipeline_; }
    uint32_t pushConstantSize() const { return pushSize_; }
    WGPUBindGroup pushBindGroup() const { return pushGroup_; }

    static uint32_t groupCount(uint32_t total, uint32_t localSize) {
        return (total + localSize - 1) / localSize;
    }

private:
    Device& device_;
    WGPUComputePipeline pipeline_ = nullptr;
    WGPUBindGroup pushGroup_ = nullptr;
    WGPUBindGroupLayout pushLayout_ = nullptr;
    uint32_t pushSize_ = 0;
};

} // namespace saida::rhi::webgpu
