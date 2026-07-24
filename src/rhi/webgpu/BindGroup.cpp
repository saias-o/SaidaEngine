#include "rhi/webgpu/BindGroup.hpp"

#include "rhi/webgpu/Buffer.hpp"
#include "rhi/webgpu/Device.hpp"
#include "rhi/webgpu/Format.hpp"

#include <stdexcept>

namespace saida::rhi::webgpu {

namespace {

WGPUShaderStage toWgpu(rhi::ShaderStages stages) {
    WGPUShaderStage flags = WGPUShaderStage_None;
    if (hasStage(stages, rhi::ShaderStages::Vertex)) flags |= WGPUShaderStage_Vertex;
    if (hasStage(stages, rhi::ShaderStages::Fragment)) flags |= WGPUShaderStage_Fragment;
    if (hasStage(stages, rhi::ShaderStages::Compute)) flags |= WGPUShaderStage_Compute;
    return flags;
}

WGPUTextureViewDimension toWgpu(TextureDim dim) {
    switch (dim) {
        case TextureDim::Dim2D:      return WGPUTextureViewDimension_2D;
        case TextureDim::Dim2DArray: return WGPUTextureViewDimension_2DArray;
        case TextureDim::Dim3D:      return WGPUTextureViewDimension_3D;
        case TextureDim::DimCube:    return WGPUTextureViewDimension_Cube;
    }
    return WGPUTextureViewDimension_2D;
}

} // namespace

BindGroupLayout::BindGroupLayout(Device& device, std::vector<BindGroupLayoutEntry> entries)
    : device_(device), entries_(std::move(entries)) {
    std::vector<WGPUBindGroupLayoutEntry> wgpu;
    wgpu.reserve(entries_.size());
    for (const BindGroupLayoutEntry& e : entries_) {
        WGPUBindGroupLayoutEntry w = {};
        w.binding = e.binding;
        w.visibility = toWgpu(e.visibility);
        switch (e.type) {
            case rhi::BindingType::UniformBuffer:
                w.buffer.type = WGPUBufferBindingType_Uniform;
                w.buffer.hasDynamicOffset = e.dynamicOffset;
                break;
            case rhi::BindingType::StorageBuffer:
                w.buffer.type = e.readOnlyStorage ? WGPUBufferBindingType_ReadOnlyStorage
                                                  : WGPUBufferBindingType_Storage;
                break;
            case rhi::BindingType::SampledTexture:
                w.texture.viewDimension = toWgpu(e.dim);
                w.texture.sampleType = e.depthTexture ? WGPUTextureSampleType_Depth
                                     : e.unfilterable ? WGPUTextureSampleType_UnfilterableFloat
                                                      : WGPUTextureSampleType_Float;
                break;
            case rhi::BindingType::Sampler:
                w.sampler.type = e.comparisonSampler   ? WGPUSamplerBindingType_Comparison
                               : e.nonFilteringSampler ? WGPUSamplerBindingType_NonFiltering
                                                       : WGPUSamplerBindingType_Filtering;
                break;
            case rhi::BindingType::StorageImage:
                w.storageTexture.access = e.storageAccess;
                w.storageTexture.format = toWgpu(e.storageFormat);
                w.storageTexture.viewDimension = toWgpu(e.dim);
                break;
            case rhi::BindingType::CombinedImageSampler:
                throw std::runtime_error("webgpu: combined image samplers do not exist on web");
        }
        wgpu.push_back(w);
    }

    WGPUBindGroupLayoutDescriptor desc = {};
    desc.entryCount = wgpu.size();
    desc.entries = wgpu.data();
    layout_ = wgpuDeviceCreateBindGroupLayout(device_.device(), &desc);
    if (!layout_) throw std::runtime_error("webgpu: failed to create bind group layout");
}

BindGroupLayout::~BindGroupLayout() {
    if (layout_) wgpuBindGroupLayoutRelease(layout_);
}

BindGroup::BindGroup(BindGroupLayout& layout, const std::vector<BindGroupEntry>& entries) {
    std::vector<WGPUBindGroupEntry> wgpu;
    wgpu.reserve(entries.size());
    for (const BindGroupEntry& e : entries) {
        WGPUBindGroupEntry w = {};
        w.binding = e.binding;
        if (e.buffer) {
            w.buffer = e.buffer->handle();
            w.offset = e.offset;
            w.size = e.range == 0 ? WGPU_WHOLE_SIZE : e.range;
        }
        if (e.view) w.textureView = e.view;
        if (e.sampler) w.sampler = e.sampler;
        wgpu.push_back(w);
    }

    WGPUBindGroupDescriptor desc = {};
    desc.layout = layout.handle();
    desc.entryCount = wgpu.size();
    desc.entries = wgpu.data();
    group_ = wgpuDeviceCreateBindGroup(layout.device().device(), &desc);
    if (!group_) throw std::runtime_error("webgpu: failed to create bind group");
}

BindGroup::~BindGroup() {
    if (group_) wgpuBindGroupRelease(group_);
}

} // namespace saida::rhi::webgpu
