#pragma once

#include "rhi/BindGroup.hpp"
#include "rhi/Format.hpp"
#include "rhi/ShaderStages.hpp"
#include "rhi/webgpu/WebGpu.hpp"

#include <vector>

// WebGPU layouts retain texture and sampler metadata required at pipeline creation.

namespace saida::rhi::webgpu {

class Buffer;
class Device;

enum class TextureDim : uint32_t { Dim2D, Dim2DArray, Dim3D, DimCube };

struct BindGroupLayoutEntry {
    uint32_t binding = 0;
    rhi::BindingType type = rhi::BindingType::UniformBuffer;
    rhi::ShaderStages visibility = rhi::ShaderStages::VertexFragment;

    TextureDim dim = TextureDim::Dim2D;
    bool depthTexture = false;
    bool unfilterable = false;

    bool comparisonSampler = false;
    bool nonFilteringSampler = false;

    bool readOnlyStorage = false;
    bool dynamicOffset = false;

    rhi::Format storageFormat = rhi::Format::RGBA16Float;
    WGPUStorageTextureAccess storageAccess = WGPUStorageTextureAccess_ReadWrite;
};

class BindGroupLayout {
public:
    BindGroupLayout(Device& device, std::vector<BindGroupLayoutEntry> entries);
    ~BindGroupLayout();
    BindGroupLayout(const BindGroupLayout&) = delete;
    BindGroupLayout& operator=(const BindGroupLayout&) = delete;

    WGPUBindGroupLayout handle() const { return layout_; }
    const std::vector<BindGroupLayoutEntry>& entries() const { return entries_; }
    Device& device() const { return device_; }

private:
    Device& device_;
    std::vector<BindGroupLayoutEntry> entries_;
    WGPUBindGroupLayout layout_ = nullptr;
};

struct BindGroupEntry {
    uint32_t binding = 0;
    const Buffer* buffer = nullptr;
    uint64_t offset = 0;
    uint64_t range = 0;               // 0 = whole buffer
    WGPUTextureView view = nullptr;   // sampled/storage textures
    WGPUSampler sampler = nullptr;
};

class BindGroup {
public:
    BindGroup(BindGroupLayout& layout, const std::vector<BindGroupEntry>& entries);
    ~BindGroup();
    BindGroup(const BindGroup&) = delete;
    BindGroup& operator=(const BindGroup&) = delete;

    WGPUBindGroup handle() const { return group_; }

private:
    WGPUBindGroup group_ = nullptr;
};

} // namespace saida::rhi::webgpu
