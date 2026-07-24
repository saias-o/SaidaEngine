#include "rhi/webgpu/RenderTexture.hpp"

#include "rhi/webgpu/Device.hpp"
#include "rhi/webgpu/Format.hpp"

#include <stdexcept>

namespace saida::rhi::webgpu {

namespace {

WGPUTextureUsage toWgpu(rhi::TextureUsage usage) {
    WGPUTextureUsage flags = WGPUTextureUsage_None;
    if (hasUsage(usage, rhi::TextureUsage::Sampled)) flags |= WGPUTextureUsage_TextureBinding;
    if (hasUsage(usage, rhi::TextureUsage::Storage)) flags |= WGPUTextureUsage_StorageBinding;
    if (hasUsage(usage, rhi::TextureUsage::ColorAttachment) ||
        hasUsage(usage, rhi::TextureUsage::DepthAttachment) ||
        hasUsage(usage, rhi::TextureUsage::Transient))
        flags |= WGPUTextureUsage_RenderAttachment;
    if (hasUsage(usage, rhi::TextureUsage::CopySrc)) flags |= WGPUTextureUsage_CopySrc;
    if (hasUsage(usage, rhi::TextureUsage::CopyDst)) flags |= WGPUTextureUsage_CopyDst;
    return flags;
}

} // namespace

RenderTexture::RenderTexture(Device& device, const RenderTextureDesc& desc)
    : device_(device), desc_(desc) {
    const bool is3d = desc_.depth > 1;

    WGPUTextureDescriptor td = {};
    td.usage = toWgpu(desc_.usage);
    td.dimension = is3d ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
    td.size = {desc_.width, desc_.height, is3d ? desc_.depth : desc_.layers};
    td.format = toWgpu(desc_.format);
    td.mipLevelCount = 1;
    td.sampleCount = desc_.samples;
    texture_ = wgpuDeviceCreateTexture(device_.device(), &td);
    if (!texture_) throw std::runtime_error("webgpu: failed to create render texture");

    WGPUTextureViewDescriptor vd = {};
    vd.format = td.format;
    vd.dimension = is3d ? WGPUTextureViewDimension_3D
                 : desc_.layers > 1 ? WGPUTextureViewDimension_2DArray
                                    : WGPUTextureViewDimension_2D;
    vd.mipLevelCount = 1;
    vd.arrayLayerCount = is3d ? 1 : desc_.layers;
    view_ = wgpuTextureCreateView(texture_, &vd);

    if (!is3d && desc_.layers > 1) {
        layerViews_.reserve(desc_.layers);
        for (uint32_t i = 0; i < desc_.layers; ++i) {
            WGPUTextureViewDescriptor ld = {};
            ld.format = td.format;
            ld.dimension = WGPUTextureViewDimension_2D;
            ld.baseArrayLayer = i;
            ld.arrayLayerCount = 1;
            ld.mipLevelCount = 1;
            layerViews_.push_back(wgpuTextureCreateView(texture_, &ld));
        }
    }
}

RenderTexture::~RenderTexture() {
    for (WGPUTextureView view : layerViews_) wgpuTextureViewRelease(view);
    if (view_) wgpuTextureViewRelease(view_);
    if (texture_) {
        wgpuTextureDestroy(texture_);
        wgpuTextureRelease(texture_);
    }
}

} // namespace saida::rhi::webgpu
