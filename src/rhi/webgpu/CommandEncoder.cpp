#include "rhi/webgpu/CommandEncoder.hpp"

#include "rhi/webgpu/BindGroup.hpp"
#include "rhi/webgpu/Buffer.hpp"
#include "rhi/webgpu/Device.hpp"
#include "rhi/webgpu/Pipeline.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace saida::rhi::webgpu {

namespace {

WGPULoadOp toWgpu(rhi::LoadOp op) {
    switch (op) {
        case rhi::LoadOp::Load:     return WGPULoadOp_Load;
        case rhi::LoadOp::Clear:    return WGPULoadOp_Clear;
        case rhi::LoadOp::DontCare: return WGPULoadOp_Clear;  // no dont-care on web
    }
    return WGPULoadOp_Clear;
}

// IEEE-754 float32 → float16 (round-toward-zero mantissa; sufficient for the GI
// atlas init constants that are the only non-zero clears on web).
uint16_t floatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = int32_t((x >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0) return uint16_t(sign);                  // subnormal/underflow → ±0
    if (exp >= 31) return uint16_t(sign | 0x7C00u);       // overflow → ±inf
    return uint16_t(sign | (uint32_t(exp) << 10) | (mant >> 13));
}

// Encode `c` into one texel of `fmt`; returns bytes written (bpp), 0 if unsupported.
uint32_t encodeTexel(WGPUTextureFormat fmt, const std::array<float, 4>& c, uint8_t* out) {
    auto u8 = [](float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    switch (fmt) {
        case WGPUTextureFormat_RGBA8Unorm:
        case WGPUTextureFormat_RGBA8UnormSrgb:
            out[0] = u8(c[0]); out[1] = u8(c[1]); out[2] = u8(c[2]); out[3] = u8(c[3]); return 4;
        case WGPUTextureFormat_BGRA8Unorm:
        case WGPUTextureFormat_BGRA8UnormSrgb:
            out[0] = u8(c[2]); out[1] = u8(c[1]); out[2] = u8(c[0]); out[3] = u8(c[3]); return 4;
        case WGPUTextureFormat_RG16Float: {
            uint16_t h[2] = {floatToHalf(c[0]), floatToHalf(c[1])};
            std::memcpy(out, h, 4); return 4;
        }
        case WGPUTextureFormat_RGBA16Float: {
            uint16_t h[4] = {floatToHalf(c[0]), floatToHalf(c[1]), floatToHalf(c[2]), floatToHalf(c[3])};
            std::memcpy(out, h, 8); return 8;
        }
        case WGPUTextureFormat_RGBA32Float:
            std::memcpy(out, c.data(), 16); return 16;
        default: return 0;
    }
}

} // namespace

// ---- RenderPassEncoder ----

void RenderPassEncoder::setPipeline(const Pipeline& pipeline) {
    pipeline_ = &pipeline;
    wgpuRenderPassEncoderSetPipeline(pass_, pipeline.handle());
    // Fill layout gaps so validation sees every group bound.
    for (uint32_t i = 0; i + 1 < pipeline.groupCountUsed(); ++i)
        wgpuRenderPassEncoderSetBindGroup(pass_, i, device_->emptyBindGroup(), 0, nullptr);
}

void RenderPassEncoder::setBindGroup(uint32_t index, const BindGroup& group) {
    wgpuRenderPassEncoderSetBindGroup(pass_, index, group.handle(), 0, nullptr);
}

void RenderPassEncoder::setBindGroup(uint32_t index, WGPUBindGroup group) {
    wgpuRenderPassEncoderSetBindGroup(pass_, index, group, 0, nullptr);
}

void RenderPassEncoder::setPushConstants(const void* data, uint32_t size, uint32_t offset) {
    (void)offset;
    if (!pipeline_ || pipeline_->pushConstantSize() == 0) return;
    const uint32_t slot = device_->allocPushSlot(data, size);
    wgpuRenderPassEncoderSetBindGroup(pass_, 3, pipeline_->pushBindGroup(), 1, &slot);
}

void RenderPassEncoder::setViewport(float x, float y, float width, float height,
                                    float minDepth, float maxDepth) {
    wgpuRenderPassEncoderSetViewport(pass_, x, y, width, height, minDepth, maxDepth);
}

void RenderPassEncoder::setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    wgpuRenderPassEncoderSetScissorRect(pass_, uint32_t(x), uint32_t(y), width, height);
}

void RenderPassEncoder::setVertexBuffer(const Buffer& buffer, uint64_t offset) {
    wgpuRenderPassEncoderSetVertexBuffer(pass_, 0, buffer.handle(), offset,
                                         buffer.size() - offset);
}

void RenderPassEncoder::setIndexBuffer(const Buffer& buffer, rhi::IndexType type,
                                       uint64_t offset) {
    wgpuRenderPassEncoderSetIndexBuffer(
        pass_, buffer.handle(),
        type == rhi::IndexType::Uint16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32,
        offset, buffer.size() - offset);
}

void RenderPassEncoder::draw(uint32_t vertexCount, uint32_t instanceCount,
                             uint32_t firstVertex, uint32_t firstInstance) {
    wgpuRenderPassEncoderDraw(pass_, vertexCount, instanceCount, firstVertex, firstInstance);
}

void RenderPassEncoder::drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                                    uint32_t firstIndex, int32_t vertexOffset,
                                    uint32_t firstInstance) {
    wgpuRenderPassEncoderDrawIndexed(pass_, indexCount, instanceCount, firstIndex,
                                     vertexOffset, firstInstance);
}

void RenderPassEncoder::drawIndirect(const Buffer& buffer, uint64_t offset,
                                     uint32_t drawCount, uint32_t stride) {
    // WebGPU's drawIndirect takes ONE draw per call; loop for parity.
    for (uint32_t i = 0; i < drawCount; ++i)
        wgpuRenderPassEncoderDrawIndirect(pass_, buffer.handle(), offset + uint64_t(i) * stride);
}

void RenderPassEncoder::drawIndexedIndirect(const Buffer& buffer, uint64_t offset,
                                            uint32_t drawCount, uint32_t stride) {
    for (uint32_t i = 0; i < drawCount; ++i)
        wgpuRenderPassEncoderDrawIndexedIndirect(pass_, buffer.handle(),
                                                 offset + uint64_t(i) * stride);
}

void RenderPassEncoder::end() {
    wgpuRenderPassEncoderEnd(pass_);
    wgpuRenderPassEncoderRelease(pass_);
    pass_ = nullptr;
}

// ---- ComputePassEncoder ----

void ComputePassEncoder::setPipeline(const ComputePipeline& pipeline) {
    pipeline_ = &pipeline;
    wgpuComputePassEncoderSetPipeline(pass_, pipeline.handle());
}

void ComputePassEncoder::setBindGroup(uint32_t index, const BindGroup& group) {
    wgpuComputePassEncoderSetBindGroup(pass_, index, group.handle(), 0, nullptr);
}

void ComputePassEncoder::setBindGroup(uint32_t index, WGPUBindGroup group) {
    wgpuComputePassEncoderSetBindGroup(pass_, index, group, 0, nullptr);
}

void ComputePassEncoder::setPushConstants(const void* data, uint32_t size, uint32_t offset) {
    (void)offset;
    if (!pipeline_ || pipeline_->pushConstantSize() == 0) return;
    const uint32_t slot = device_->allocPushSlot(data, size);
    wgpuComputePassEncoderSetBindGroup(pass_, 3, pipeline_->pushBindGroup(), 1, &slot);
}

void ComputePassEncoder::dispatch(uint32_t x, uint32_t y, uint32_t z) {
    wgpuComputePassEncoderDispatchWorkgroups(pass_, x, y, z);
}

void ComputePassEncoder::end() {
    wgpuComputePassEncoderEnd(pass_);
    wgpuComputePassEncoderRelease(pass_);
    pass_ = nullptr;
}

// ---- CommandEncoder ----

CommandEncoder::CommandEncoder(Device& device) : device_(device) {
    encoder_ = wgpuDeviceCreateCommandEncoder(device_.device(), nullptr);
}

CommandEncoder::~CommandEncoder() {
    if (encoder_) wgpuCommandEncoderRelease(encoder_);
}

RenderPassEncoder CommandEncoder::beginRenderPass(const RenderPassDesc& desc) {
    std::vector<WGPURenderPassColorAttachment> colors;
    colors.reserve(desc.colorCount);
    for (uint32_t i = 0; i < desc.colorCount; ++i) {
        const ColorAttachment& src = desc.colors[i];
        WGPURenderPassColorAttachment color = {};
        color.view = src.view;
        color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        color.loadOp = toWgpu(src.loadOp);
        color.storeOp = src.store ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
        color.clearValue = {src.clearColor[0], src.clearColor[1], src.clearColor[2],
                            src.clearColor[3]};
        if (src.resolveView) color.resolveTarget = src.resolveView;
        colors.push_back(color);
    }

    WGPURenderPassDepthStencilAttachment depth = {};
    if (desc.depth.view) {
        depth.view = desc.depth.view;
        depth.depthLoadOp = toWgpu(desc.depth.loadOp);
        depth.depthStoreOp = desc.depth.store ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
        depth.depthClearValue = desc.depth.clearDepth;
    }

    WGPURenderPassDescriptor rp = {};
    rp.colorAttachmentCount = colors.size();
    rp.colorAttachments = colors.empty() ? nullptr : colors.data();
    rp.depthStencilAttachment = desc.depth.view ? &depth : nullptr;

    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder_, &rp);
    RenderPassEncoder encoder(pass, device_);
    if (desc.defaultViewportScissor && desc.width > 0) {
        encoder.setViewport(float(desc.x), float(desc.y), float(desc.width), float(desc.height));
        encoder.setScissor(desc.x, desc.y, desc.width, desc.height);
    }
    return encoder;
}

ComputePassEncoder CommandEncoder::beginComputePass() {
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(encoder_, nullptr);
    return ComputePassEncoder(pass, device_);
}

void CommandEncoder::copyBufferToBuffer(const Buffer& src, const Buffer& dst, uint64_t size,
                                        uint64_t srcOffset, uint64_t dstOffset) {
    wgpuCommandEncoderCopyBufferToBuffer(encoder_, src.handle(), srcOffset,
                                         dst.handle(), dstOffset, size);
}

void CommandEncoder::copyBufferToTexture(const Buffer& src, WGPUTexture dst,
                                         uint32_t width, uint32_t height,
                                         uint32_t mipLevel, uint32_t baseLayer,
                                         uint32_t layerCount, uint64_t srcOffset) {
    (void)layerCount;
    WGPUTexelCopyBufferInfo bufferInfo = {};
    bufferInfo.buffer = src.handle();
    bufferInfo.layout.offset = srcOffset;
    bufferInfo.layout.bytesPerRow = width * 4;
    bufferInfo.layout.rowsPerImage = height;
    WGPUTexelCopyTextureInfo textureInfo = {};
    textureInfo.texture = dst;
    textureInfo.mipLevel = mipLevel;
    textureInfo.origin = {0, 0, baseLayer};
    WGPUExtent3D extent = {width, height, 1};
    wgpuCommandEncoderCopyBufferToTexture(encoder_, &bufferInfo, &textureInfo, &extent);
}

void CommandEncoder::clearColorTexture(WGPUTexture texture, const std::array<float, 4>& color) {
    // WebGPU has no clear-texture command outside a render pass. Two paths:
    //  - zero: copy from the Device's cached all-zero buffer, in-encoder (the GI
    //    voxel grid is re-cleared every frame, so this stays on the fast path);
    //  - non-zero: queue.writeTexture with the constant encoded per texel (only
    //    the GI atlas init constants, uploaded once at startup — ordered before
    //    the first submit, so the no-op state transitions around it are fine).
    const WGPUTextureFormat fmt = wgpuTextureGetFormat(texture);
    const uint32_t w = wgpuTextureGetWidth(texture);
    const uint32_t h = wgpuTextureGetHeight(texture);
    const uint32_t d = wgpuTextureGetDepthOrArrayLayers(texture);
    const bool isZero = color[0] == 0.0f && color[1] == 0.0f &&
                        color[2] == 0.0f && color[3] == 0.0f;

    if (!isZero) {
        uint8_t texel[16];
        const uint32_t bpp = encodeTexel(fmt, color, texel);
        if (bpp == 0) {
            std::printf("rhi::webgpu: clearColorTexture: unsupported non-zero format %d\n", int(fmt));
            return;
        }
        std::vector<uint8_t> data(size_t(w) * h * d * bpp);
        for (size_t i = 0; i < data.size(); i += bpp) std::memcpy(&data[i], texel, bpp);
        WGPUTexelCopyTextureInfo dst = {};
        dst.texture = texture;
        WGPUTexelCopyBufferLayout layout = {};
        layout.bytesPerRow = w * bpp;      // writeTexture rows need no 256 alignment
        layout.rowsPerImage = h;
        WGPUExtent3D ext = {w, h, d};
        wgpuQueueWriteTexture(device_.queue(), &dst, data.data(), data.size(), &layout, &ext);
        return;
    }

    uint32_t bpp = 0;
    switch (fmt) {
        case WGPUTextureFormat_R32Float:
        case WGPUTextureFormat_RG16Float:
        case WGPUTextureFormat_RGBA8Unorm:
        case WGPUTextureFormat_RGBA8UnormSrgb:
        case WGPUTextureFormat_BGRA8Unorm:
        case WGPUTextureFormat_BGRA8UnormSrgb: bpp = 4; break;
        case WGPUTextureFormat_RG32Float:
        case WGPUTextureFormat_RGBA16Float:   bpp = 8; break;
        case WGPUTextureFormat_RGBA32Float:   bpp = 16; break;
        default:
            std::printf("rhi::webgpu: clearColorTexture: unsupported format %d\n", int(fmt));
            return;
    }

    const uint32_t bytesPerRow = (w * bpp + 255u) & ~255u;  // copy alignment

    WGPUTexelCopyBufferInfo src = {};
    src.buffer = device_.zeroBuffer(uint64_t(bytesPerRow) * h * d);
    src.layout.bytesPerRow = bytesPerRow;
    src.layout.rowsPerImage = h;
    WGPUTexelCopyTextureInfo dst = {};
    dst.texture = texture;
    WGPUExtent3D extent = {w, h, d};
    wgpuCommandEncoderCopyBufferToTexture(encoder_, &src, &dst, &extent);
}

void CommandEncoder::fillBuffer(const Buffer& dst, uint64_t offset, uint64_t size,
                                uint32_t value) {
    if (value == 0) {
        wgpuCommandEncoderClearBuffer(encoder_, dst.handle(), offset, size);
    } else {
        std::printf("rhi::webgpu: fillBuffer only supports zero fills\n");
    }
}

WGPUCommandBuffer CommandEncoder::finish() {
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder_, nullptr);
    wgpuCommandEncoderRelease(encoder_);
    encoder_ = nullptr;
    return cmd;
}

} // namespace saida::rhi::webgpu
