#pragma once

#include "rhi/Format.hpp"
#include "rhi/TextureUsage.hpp"
#include "rhi/webgpu/WebGpu.hpp"

#include <string>
#include <vector>


namespace saida::rhi::webgpu {

class Device;

struct RenderTextureDesc {
    rhi::Format format = rhi::Format::Undefined;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t layers = 1;
    uint32_t samples = 1;
    rhi::TextureUsage usage = rhi::TextureUsage::None;
    std::string memoryCategory;  // accepted for API parity; no profiler on web yet
};

class RenderTexture {
public:
    RenderTexture(Device& device, const RenderTextureDesc& desc);
    ~RenderTexture();
    RenderTexture(const RenderTexture&) = delete;
    RenderTexture& operator=(const RenderTexture&) = delete;

    WGPUTexture image() const { return texture_; }
    WGPUTextureView view() const { return view_; }
    WGPUTextureView layerView(uint32_t layer) const { return layerViews_[layer]; }

    rhi::Format format() const { return desc_.format; }
    uint32_t width() const { return desc_.width; }
    uint32_t height() const { return desc_.height; }
    uint32_t layers() const { return desc_.layers; }

private:
    Device& device_;
    RenderTextureDesc desc_;
    WGPUTexture texture_ = nullptr;
    WGPUTextureView view_ = nullptr;
    std::vector<WGPUTextureView> layerViews_;
};

} // namespace saida::rhi::webgpu
