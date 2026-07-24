#include "rhi/webgpu/Pipeline.hpp"

#include "rhi/webgpu/BindGroup.hpp"
#include "rhi/webgpu/Device.hpp"
#include "rhi/webgpu/Format.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace saida::rhi::webgpu {

namespace {

std::string readTextFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open())
        throw std::runtime_error("webgpu: failed to open shader " + path);
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

WGPUShaderModule createModule(WGPUDevice device, const std::string& path) {
    const std::string source = readTextFile(path);
    WGPUShaderSourceWGSL wgsl = {};
    wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl.code.data = source.data();
    wgsl.code.length = source.size();
    WGPUShaderModuleDescriptor desc = {};
    desc.nextInChain = &wgsl.chain;
    desc.label = sv(path.c_str());
    return wgpuDeviceCreateShaderModule(device, &desc);
}

WGPUCompareFunction toWgpu(rhi::CompareOp op) {
    switch (op) {
        case rhi::CompareOp::Never:          return WGPUCompareFunction_Never;
        case rhi::CompareOp::Less:           return WGPUCompareFunction_Less;
        case rhi::CompareOp::Equal:          return WGPUCompareFunction_Equal;
        case rhi::CompareOp::LessOrEqual:    return WGPUCompareFunction_LessEqual;
        case rhi::CompareOp::Greater:        return WGPUCompareFunction_Greater;
        case rhi::CompareOp::NotEqual:       return WGPUCompareFunction_NotEqual;
        case rhi::CompareOp::GreaterOrEqual: return WGPUCompareFunction_GreaterEqual;
        case rhi::CompareOp::Always:         return WGPUCompareFunction_Always;
    }
    return WGPUCompareFunction_Less;
}

WGPUPrimitiveTopology toWgpu(rhi::Topology topology) {
    switch (topology) {
        case rhi::Topology::TriangleList:  return WGPUPrimitiveTopology_TriangleList;
        case rhi::Topology::TriangleStrip: return WGPUPrimitiveTopology_TriangleStrip;
        case rhi::Topology::LineList:      return WGPUPrimitiveTopology_LineList;
        case rhi::Topology::PointList:     return WGPUPrimitiveTopology_PointList;
    }
    return WGPUPrimitiveTopology_TriangleList;
}

WGPUCullMode toWgpu(rhi::CullMode mode) {
    switch (mode) {
        case rhi::CullMode::None:  return WGPUCullMode_None;
        case rhi::CullMode::Front: return WGPUCullMode_Front;
        case rhi::CullMode::Back:  return WGPUCullMode_Back;
    }
    return WGPUCullMode_Back;
}

// The engine Vertex layout (mirror of saida::Vertex in graphics/Mesh.hpp —
// pos, normal, color, texCoord, lightmapUV, tangent, boneIndices, boneWeights).
struct VertexLayout {
    WGPUVertexAttribute attributes[8];
    WGPUVertexBufferLayout layout;

    VertexLayout() {
        auto set = [&](int i, uint32_t location, WGPUVertexFormat format, uint64_t offset) {
            attributes[i] = {};
            attributes[i].shaderLocation = location;
            attributes[i].format = format;
            attributes[i].offset = offset;
        };
        set(0, 0, WGPUVertexFormat_Float32x3, 0);    // pos
        set(1, 1, WGPUVertexFormat_Float32x3, 12);   // normal
        set(2, 2, WGPUVertexFormat_Float32x3, 24);   // color
        set(3, 3, WGPUVertexFormat_Float32x2, 36);   // texCoord
        set(4, 4, WGPUVertexFormat_Float32x2, 44);   // lightmapUV
        set(5, 5, WGPUVertexFormat_Float32x4, 52);   // tangent
        set(6, 6, WGPUVertexFormat_Sint32x4, 68);    // boneIndices
        set(7, 7, WGPUVertexFormat_Float32x4, 84);   // boneWeights
        layout = {};
        layout.stepMode = WGPUVertexStepMode_Vertex;
        layout.arrayStride = 100;                    // sizeof(saida::Vertex)
        layout.attributeCount = 8;
        layout.attributes = attributes;
    }
};

// Builds group 3 (the push-constant emulation slice) and the full pipeline
// layout with empty layouts plugging the gaps. Shared by render + compute.
struct BuiltLayout {
    WGPUPipelineLayout pipelineLayout = nullptr;
    WGPUBindGroupLayout pushLayout = nullptr;
    WGPUBindGroup pushGroup = nullptr;
    uint32_t groupCount = 0;
};

BuiltLayout buildLayout(Device& device, const std::vector<const BindGroupLayout*>& layouts,
                        uint32_t pushSize, rhi::ShaderStages pushStages) {
    BuiltLayout built;

    std::vector<WGPUBindGroupLayout> groups;
    for (const BindGroupLayout* layout : layouts)
        groups.push_back(layout ? layout->handle() : device.emptyBindGroupLayout());

    if (pushSize > 0) {
        // `PUSH_QUALIFIER` puts the push block at @group(3) @binding(0).
        while (groups.size() < 3) groups.push_back(device.emptyBindGroupLayout());

        // WGSL rounds a uniform block's size up to 16 (e.g. mat4 + uint = 68
        // bytes in C++ but 80 in the shader): bind the rounded size or Dawn
        // rejects the pipeline (minBindingSize too small).
        pushSize = (pushSize + 15u) & ~15u;

        WGPUBindGroupLayoutEntry entry = {};
        entry.binding = 0;
        entry.visibility = WGPUShaderStage_None;
        if (hasStage(pushStages, rhi::ShaderStages::Vertex)) entry.visibility |= WGPUShaderStage_Vertex;
        if (hasStage(pushStages, rhi::ShaderStages::Fragment)) entry.visibility |= WGPUShaderStage_Fragment;
        if (hasStage(pushStages, rhi::ShaderStages::Compute)) entry.visibility |= WGPUShaderStage_Compute;
        entry.buffer.type = WGPUBufferBindingType_Uniform;
        entry.buffer.hasDynamicOffset = true;
        entry.buffer.minBindingSize = pushSize;

        WGPUBindGroupLayoutDescriptor ld = {};
        ld.entryCount = 1;
        ld.entries = &entry;
        built.pushLayout = wgpuDeviceCreateBindGroupLayout(device.device(), &ld);
        groups.push_back(built.pushLayout);

        WGPUBindGroupEntry ge = {};
        ge.binding = 0;
        ge.buffer = device.pushRingBuffer();
        ge.offset = 0;                 // dynamic offset selects the slot
        ge.size = pushSize;
        WGPUBindGroupDescriptor gd = {};
        gd.layout = built.pushLayout;
        gd.entryCount = 1;
        gd.entries = &ge;
        built.pushGroup = wgpuDeviceCreateBindGroup(device.device(), &gd);
    }

    WGPUPipelineLayoutDescriptor pld = {};
    pld.bindGroupLayoutCount = groups.size();
    pld.bindGroupLayouts = groups.data();
    built.pipelineLayout = wgpuDeviceCreatePipelineLayout(device.device(), &pld);
    built.groupCount = uint32_t(groups.size());
    return built;
}

} // namespace

Pipeline::Pipeline(Device& device, const Desc& desc) : device_(device) {
    BuiltLayout built = buildLayout(device_, desc.bindGroupLayouts,
                                    desc.pushConstantSize, desc.pushConstantStages);
    pushLayout_ = built.pushLayout;
    pushGroup_ = built.pushGroup;
    pushSize_ = desc.pushConstantSize;
    groupCount_ = built.groupCount;

    WGPUShaderModule vert = createModule(device_.device(), desc.vertPath);
    WGPUShaderModule frag = nullptr;

    WGPURenderPipelineDescriptor pd = {};
    pd.layout = built.pipelineLayout;
    pd.vertex.module = vert;
    pd.vertex.entryPoint = sv("main");

    VertexLayout vertexLayout;
    if (desc.vertexInput) {
        pd.vertex.bufferCount = 1;
        pd.vertex.buffers = &vertexLayout.layout;
    }

    pd.primitive.topology = toWgpu(desc.topology);
    pd.primitive.cullMode = toWgpu(desc.cullMode);
    pd.primitive.frontFace = WGPUFrontFace_CCW;

    WGPUDepthStencilState depth = {};
    if (desc.depthFormat != rhi::Format::Undefined) {
        depth.format = toWgpu(desc.depthFormat);
        depth.depthWriteEnabled = (desc.depthTest && desc.depthWrite)
                                      ? WGPUOptionalBool_True
                                      : WGPUOptionalBool_False;
        depth.depthCompare = desc.depthTest ? toWgpu(desc.depthCompare)
                                            : WGPUCompareFunction_Always;
        depth.stencilFront.compare = WGPUCompareFunction_Always;
        depth.stencilBack.compare = WGPUCompareFunction_Always;
        depth.stencilReadMask = 0xFFFFFFFF;
        depth.stencilWriteMask = 0xFFFFFFFF;
        if (desc.depthBias) {
            depth.depthBias = int32_t(desc.depthBiasConstant);
            depth.depthBiasSlopeScale = desc.depthBiasSlope;
        }
        pd.depthStencil = &depth;
    }

    pd.multisample.count = desc.samples;
    pd.multisample.mask = 0xFFFFFFFF;

    WGPUBlendState blend = {};
    std::vector<WGPUColorTargetState> targets;
    WGPUFragmentState fragment = {};
    if (!desc.fragPath.empty()) {
        frag = createModule(device_.device(), desc.fragPath);
        fragment.module = frag;
        fragment.entryPoint = sv("main");

        if (desc.blendMode == rhi::BlendMode::Alpha) {
            blend.color = {WGPUBlendOperation_Add, WGPUBlendFactor_SrcAlpha,
                           WGPUBlendFactor_OneMinusSrcAlpha};
            blend.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_One,
                           WGPUBlendFactor_OneMinusSrcAlpha};
        } else if (desc.blendMode == rhi::BlendMode::Additive) {
            blend.color = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_One};
            blend.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_One};
        }

        for (rhi::Format format : desc.colorFormats) {
            WGPUColorTargetState target = {};
            target.format = toWgpu(format);
            target.writeMask = desc.colorWrite ? WGPUColorWriteMask_All
                                               : WGPUColorWriteMask_None;
            if (desc.blendMode != rhi::BlendMode::None) target.blend = &blend;
            targets.push_back(target);
        }
        fragment.targetCount = targets.size();
        fragment.targets = targets.data();
        pd.fragment = &fragment;
    }

    pipeline_ = wgpuDeviceCreateRenderPipeline(device_.device(), &pd);

    wgpuPipelineLayoutRelease(built.pipelineLayout);
    if (frag) wgpuShaderModuleRelease(frag);
    wgpuShaderModuleRelease(vert);
    if (!pipeline_) throw std::runtime_error("webgpu: failed to create render pipeline for " + desc.vertPath);
}

Pipeline::~Pipeline() {
    if (pipeline_) wgpuRenderPipelineRelease(pipeline_);
    if (pushGroup_) wgpuBindGroupRelease(pushGroup_);
    if (pushLayout_) wgpuBindGroupLayoutRelease(pushLayout_);
}

ComputePipeline::ComputePipeline(Device& device, const std::string& compPath,
                                 const std::vector<const BindGroupLayout*>& setLayouts,
                                 uint32_t pushConstantSize)
    : device_(device) {
    BuiltLayout built = buildLayout(device_, setLayouts, pushConstantSize,
                                    rhi::ShaderStages::Compute);
    pushLayout_ = built.pushLayout;
    pushGroup_ = built.pushGroup;
    pushSize_ = pushConstantSize;

    WGPUShaderModule module = createModule(device_.device(), compPath);
    WGPUComputePipelineDescriptor pd = {};
    pd.layout = built.pipelineLayout;
    pd.compute.module = module;
    pd.compute.entryPoint = sv("main");
    pipeline_ = wgpuDeviceCreateComputePipeline(device_.device(), &pd);

    wgpuPipelineLayoutRelease(built.pipelineLayout);
    wgpuShaderModuleRelease(module);
    if (!pipeline_) throw std::runtime_error("webgpu: failed to create compute pipeline for " + compPath);
}

ComputePipeline::~ComputePipeline() {
    if (pipeline_) wgpuComputePipelineRelease(pipeline_);
    if (pushGroup_) wgpuBindGroupRelease(pushGroup_);
    if (pushLayout_) wgpuBindGroupLayoutRelease(pushLayout_);
}

} // namespace saida::rhi::webgpu
