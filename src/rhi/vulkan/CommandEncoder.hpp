#pragma once

#include "rhi/CommandTypes.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

// Non-owning VkCommandBuffer view lets RHI and raw Vulkan record into one frame.

namespace saida {
class Buffer;
class ComputePipeline;
class Pipeline;
}

namespace saida::rhi::vulkan {

class BindGroup;

struct ColorAttachment {
    VkImageView view = VK_NULL_HANDLE;
    rhi::LoadOp loadOp = rhi::LoadOp::Clear;
    std::array<float, 4> clearColor{{0.0f, 0.0f, 0.0f, 1.0f}};
    bool store = true;
    VkImageView resolveView = VK_NULL_HANDLE;
};

struct DepthAttachment {
    VkImageView view = VK_NULL_HANDLE;
    rhi::LoadOp loadOp = rhi::LoadOp::Clear;
    float clearDepth = 1.0f;
    bool store = true;
    VkImageView resolveView = VK_NULL_HANDLE;
};

struct RenderPassDesc {
    std::array<ColorAttachment, 4> colors{};
    uint32_t colorCount = 0;
    DepthAttachment depth{};
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
    uint32_t layerCount = 1;
    uint32_t viewMask = 0;
    bool defaultViewportScissor = true;
};

class RenderPassEncoder {
public:
    void setPipeline(const saida::Pipeline& pipeline);
    void setBindGroup(uint32_t index, const BindGroup& group);
    void setBindGroup(uint32_t index, VkDescriptorSet set);
    void setPushConstants(const void* data, uint32_t size, uint32_t offset = 0);
    void setViewport(float x, float y, float width, float height,
                     float minDepth = 0.0f, float maxDepth = 1.0f);
    void setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height);
    void setVertexBuffer(const saida::Buffer& buffer, uint64_t offset = 0);
    void setIndexBuffer(const saida::Buffer& buffer,
                        rhi::IndexType type = rhi::IndexType::Uint32, uint64_t offset = 0);
    void draw(uint32_t vertexCount, uint32_t instanceCount = 1,
              uint32_t firstVertex = 0, uint32_t firstInstance = 0);
    void drawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0,
                     int32_t vertexOffset = 0, uint32_t firstInstance = 0);
    void drawIndirect(const saida::Buffer& buffer, uint64_t offset,
                      uint32_t drawCount, uint32_t stride);
    void drawIndexedIndirect(const saida::Buffer& buffer, uint64_t offset,
                             uint32_t drawCount, uint32_t stride);
    // Callers fall back to drawIndexedIndirect when indirect-count is unavailable.
    void drawIndexedIndirectCount(const saida::Buffer& args, uint64_t argsOffset,
                                  const saida::Buffer& count, uint64_t countOffset,
                                  uint32_t maxDrawCount, uint32_t stride);
    void end();

    VkCommandBuffer handle() const { return cmd_; }

    // Lets RHI code record into a dynamic-rendering pass begun through raw Vulkan.
    static RenderPassEncoder fromHandle(VkCommandBuffer cmd) { return RenderPassEncoder(cmd); }

private:
    friend class CommandEncoder;
    explicit RenderPassEncoder(VkCommandBuffer cmd) : cmd_(cmd) {}

    VkCommandBuffer cmd_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderStageFlags pushStages_ = 0;
};

class ComputePassEncoder {
public:
    void setPipeline(const saida::ComputePipeline& pipeline);
    void setBindGroup(uint32_t index, const BindGroup& group);
    void setBindGroup(uint32_t index, VkDescriptorSet set);
    void setPushConstants(const void* data, uint32_t size, uint32_t offset = 0);
    void dispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1);
    void end() {}

    VkCommandBuffer handle() const { return cmd_; }

private:
    friend class CommandEncoder;
    explicit ComputePassEncoder(VkCommandBuffer cmd) : cmd_(cmd) {}

    VkCommandBuffer cmd_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
};

class CommandEncoder {
public:
    explicit CommandEncoder(VkCommandBuffer cmd) : cmd_(cmd) {}

    VkCommandBuffer handle() const { return cmd_; }

    // The caller states where the image was and where it goes. `discardContents`
    // keeps the source sync scope but drops the contents (oldLayout UNDEFINED),
    // for targets fully overwritten by the pass (shadow layers, HDR targets).
    void transition(VkImage image, rhi::ResourceState from, rhi::ResourceState to,
                    uint32_t baseLayer = 0, uint32_t layerCount = VK_REMAINING_ARRAY_LAYERS,
                    bool discardContents = false,
                    rhi::TextureAspect aspect = rhi::TextureAspect::Auto);

    void storageBarrier();

    // WebGPU tracks this dependency automatically, so its equivalent is a no-op.
    void computeToGraphicsBarrier();

    void computeToIndirectBarrier();

    void transferToComputeBarrier();

    void fillBuffer(const saida::Buffer& dst, uint64_t offset, uint64_t size, uint32_t value);

    RenderPassEncoder beginRenderPass(const RenderPassDesc& desc);
    ComputePassEncoder beginComputePass() { return ComputePassEncoder(cmd_); }

    void copyBufferToBuffer(const saida::Buffer& src, const saida::Buffer& dst, uint64_t size,
                            uint64_t srcOffset = 0, uint64_t dstOffset = 0);

    // The caller transitions the destination to CopyDst around this upload.
    void copyBufferToTexture(const saida::Buffer& src, VkImage dst,
                             uint32_t width, uint32_t height,
                             uint32_t mipLevel = 0, uint32_t baseLayer = 0,
                             uint32_t layerCount = 1, uint64_t srcOffset = 0);

    // The image must be in CopyDst before this clear.
    void clearColorTexture(VkImage image, const std::array<float, 4>& color);

private:
    VkCommandBuffer cmd_;
};

} // namespace saida::rhi::vulkan
