#pragma once

#include "rhi/Format.hpp"
#include "rhi/webgpu/CommandEncoder.hpp"
#include "rhi/webgpu/Handles.hpp"
#include "rhi/webgpu/RenderTexture.hpp"
#include "rhi/webgpu/WebGpu.hpp"

#include <memory>

// Browser presentation is implicit after the RAF callback; frame sync is driver-managed.

namespace saida::rhi::webgpu {

class Device;

class Surface {
public:
    Surface(Device& device, const char* canvasSelector, uint32_t width, uint32_t height);
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    Extent2D extent() const { return {width_, height_}; }
    rhi::Format colorFormat() const { return rhi::Format::BGRA8Unorm; }
    rhi::Format depthFormat() const { return rhi::Format::Depth32Float; }
    uint32_t samples() const { return 1; }

    void waitFrame(uint32_t) const {}
    // A hidden tab can expose no texture; the caller skips that frame.
    bool acquire(uint32_t frame, uint32_t& imageIndex);
    bool submitAndPresent(WGPUCommandBuffer cmd, uint32_t frame, uint32_t imageIndex);
    CommandEncoder beginFrameCommands(uint32_t frame);
    bool submitAndPresent(CommandEncoder& encoder, uint32_t frame, uint32_t imageIndex);

    // Valid only between acquire and submit.
    WGPUTextureView currentView() const { return currentView_; }
    WGPUTexture image(uint32_t) const { return currentTexture_; }
    WGPUTextureView imageView(uint32_t) const { return currentView_; }
    WGPUTexture depthImage() const { return depth_ ? depth_->image() : nullptr; }
    WGPUTextureView depthView() const { return depth_ ? depth_->view() : nullptr; }

private:
    Device& device_;
    WGPUSurface surface_ = nullptr;
    WGPUTexture currentTexture_ = nullptr;
    WGPUTextureView currentView_ = nullptr;
    std::unique_ptr<RenderTexture> depth_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

} // namespace saida::rhi::webgpu
