#include "rhi/webgpu/Surface.hpp"

#include "rhi/webgpu/Device.hpp"

#include <stdexcept>

namespace saida::rhi::webgpu {

Surface::Surface(Device& device, const char* canvasSelector, uint32_t width, uint32_t height)
    : device_(device), width_(width), height_(height) {
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = {};
    canvas.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    canvas.selector = sv(canvasSelector);
    WGPUSurfaceDescriptor desc = {};
    desc.nextInChain = &canvas.chain;
    surface_ = wgpuInstanceCreateSurface(device_.instance(), &desc);
    if (!surface_) throw std::runtime_error("webgpu: failed to create canvas surface");

    WGPUSurfaceConfiguration config = {};
    config.device = device_.device();
    config.format = WGPUTextureFormat_BGRA8Unorm;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = width_;
    config.height = height_;
    config.alphaMode = WGPUCompositeAlphaMode_Auto;
    config.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(surface_, &config);

    RenderTextureDesc depthDesc;
    depthDesc.format = rhi::Format::Depth32Float;
    depthDesc.width = width_;
    depthDesc.height = height_;
    depthDesc.usage = rhi::TextureUsage::DepthAttachment | rhi::TextureUsage::Sampled;
    depth_ = std::make_unique<RenderTexture>(device_, depthDesc);
}

Surface::~Surface() {
    if (currentView_) wgpuTextureViewRelease(currentView_);
    if (surface_) wgpuSurfaceRelease(surface_);
}

bool Surface::acquire(uint32_t frame, uint32_t& imageIndex) {
    (void)frame;
    imageIndex = 0;
    device_.beginFrame();  // reset the push-constant ring for this frame

    WGPUSurfaceTexture st = {};
    wgpuSurfaceGetCurrentTexture(surface_, &st);
    if (!st.texture) return false;
    if (currentView_) wgpuTextureViewRelease(currentView_);
    currentTexture_ = st.texture;
    currentView_ = wgpuTextureCreateView(st.texture, nullptr);
    return true;
}

bool Surface::submitAndPresent(WGPUCommandBuffer cmd, uint32_t frame, uint32_t imageIndex) {
    (void)frame;
    (void)imageIndex;
    device_.flushPushRing();  // slots recorded this frame, before their consumers
    wgpuQueueSubmit(device_.queue(), 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    return false;  // browser handles resize/composition; nothing to recreate here
}

CommandEncoder Surface::beginFrameCommands(uint32_t frame) {
    (void)frame;
    return CommandEncoder(device_);
}

bool Surface::submitAndPresent(CommandEncoder& encoder, uint32_t frame, uint32_t imageIndex) {
    return submitAndPresent(encoder.finish(), frame, imageIndex);
}

} // namespace saida::rhi::webgpu
