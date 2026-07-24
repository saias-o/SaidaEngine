#pragma once

#include "rhi/CommandTypes.hpp"
#include "rhi/webgpu/WebGpu.hpp"

#include <array>
#include <cstdint>

// WebGPU tracks resource state itself, so transitions and barriers are no-ops.
// Push constants use a dynamic slot in the device ring.

namespace saida::rhi::webgpu {

class BindGroup;
class Buffer;
class ComputePipeline;
class Device;
class Pipeline;

struct ColorAttachment {
    WGPUTextureView view = nullptr;
    rhi::LoadOp loadOp = rhi::LoadOp::Clear;
    std::array<float, 4> clearColor{{0.0f, 0.0f, 0.0f, 1.0f}};
    bool store = true;
    WGPUTextureView resolveView = nullptr;  // MSAA resolve target
};

struct DepthAttachment {
    WGPUTextureView view = nullptr;
    rhi::LoadOp loadOp = rhi::LoadOp::Clear;
    float clearDepth = 1.0f;
    bool store = true;
    WGPUTextureView resolveView = nullptr;  // no depth resolve on web (ignored)
};

struct RenderPassDesc {
    std::array<ColorAttachment, 4> colors{};
    uint32_t colorCount = 0;
    DepthAttachment depth{};
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
    uint32_t layerCount = 1;
    uint32_t viewMask = 0;              // no multiview on web
    bool defaultViewportScissor = true;
};

class RenderPassEncoder {
public:
    void setPipeline(const Pipeline& pipeline);
    void setBindGroup(uint32_t index, const BindGroup& group);
    void setBindGroup(uint32_t index, WGPUBindGroup group);
    void setPushConstants(const void* data, uint32_t size, uint32_t offset = 0);
    void setViewport(float x, float y, float width, float height,
                     float minDepth = 0.0f, float maxDepth = 1.0f);
    void setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height);
    void setVertexBuffer(const Buffer& buffer, uint64_t offset = 0);
    void setIndexBuffer(const Buffer& buffer,
                        rhi::IndexType type = rhi::IndexType::Uint32, uint64_t offset = 0);
    void draw(uint32_t vertexCount, uint32_t instanceCount = 1,
              uint32_t firstVertex = 0, uint32_t firstInstance = 0);
    void drawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0,
                     int32_t vertexOffset = 0, uint32_t firstInstance = 0);
    void drawIndirect(const Buffer& buffer, uint64_t offset, uint32_t drawCount, uint32_t stride);
    void drawIndexedIndirect(const Buffer& buffer, uint64_t offset,
                             uint32_t drawCount, uint32_t stride);
    void end();

private:
    friend class CommandEncoder;
    RenderPassEncoder(WGPURenderPassEncoder pass, Device& device)
        : pass_(pass), device_(&device) {}

    WGPURenderPassEncoder pass_;
    Device* device_;
    const Pipeline* pipeline_ = nullptr;
};

class ComputePassEncoder {
public:
    void setPipeline(const ComputePipeline& pipeline);
    void setBindGroup(uint32_t index, const BindGroup& group);
    void setBindGroup(uint32_t index, WGPUBindGroup group);
    void setPushConstants(const void* data, uint32_t size, uint32_t offset = 0);
    void dispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1);
    void end();

private:
    friend class CommandEncoder;
    ComputePassEncoder(WGPUComputePassEncoder pass, Device& device)
        : pass_(pass), device_(&device) {}

    WGPUComputePassEncoder pass_;
    Device* device_;
    const ComputePipeline* pipeline_ = nullptr;
};

class CommandEncoder {
public:
    explicit CommandEncoder(Device& device);
    ~CommandEncoder();
    CommandEncoder(const CommandEncoder&) = delete;
    CommandEncoder& operator=(const CommandEncoder&) = delete;

    // Barriers/transitions: driver-tracked on WebGPU → no-ops by design.
    void transition(WGPUTexture, rhi::ResourceState, rhi::ResourceState,
                    uint32_t = 0, uint32_t = ~0u, bool = false,
                    rhi::TextureAspect = rhi::TextureAspect::Auto) {}
    void storageBarrier() {}
    void computeToGraphicsBarrier() {}
    void computeToIndirectBarrier() {}
    void transferToComputeBarrier() {}

    RenderPassEncoder beginRenderPass(const RenderPassDesc& desc);
    ComputePassEncoder beginComputePass();

    void copyBufferToBuffer(const Buffer& src, const Buffer& dst, uint64_t size,
                            uint64_t srcOffset = 0, uint64_t dstOffset = 0);
    void copyBufferToTexture(const Buffer& src, WGPUTexture dst,
                             uint32_t width, uint32_t height,
                             uint32_t mipLevel = 0, uint32_t baseLayer = 0,
                             uint32_t layerCount = 1, uint64_t srcOffset = 0);
    void clearColorTexture(WGPUTexture texture, const std::array<float, 4>& color);
    void fillBuffer(const Buffer& dst, uint64_t offset, uint64_t size, uint32_t value);

    // Ends recording; caller owns the returned command buffer (Surface submits).
    WGPUCommandBuffer finish();

    WGPUCommandEncoder handle() const { return encoder_; }

private:
    Device& device_;
    WGPUCommandEncoder encoder_ = nullptr;
};

} // namespace saida::rhi::webgpu
