#include "rhi/vulkan/CommandEncoder.hpp"

#include "graphics/Buffer.hpp"
#include "graphics/ComputePipeline.hpp"
#include "graphics/GpuSync.hpp"
#include "graphics/Pipeline.hpp"
#include "rhi/vulkan/BindGroup.hpp"
#include "rhi/vulkan/Convert.hpp"

namespace saida::rhi::vulkan {

// ---- RenderPassEncoder ----

void RenderPassEncoder::setPipeline(const saida::Pipeline& pipeline) {
    pipeline.bind(cmd_);
    layout_ = pipeline.layout();
    pushStages_ = pipeline.pushConstantStages();
}

void RenderPassEncoder::setBindGroup(uint32_t index, const BindGroup& group) {
    setBindGroup(index, group.handle());
}

void RenderPassEncoder::setBindGroup(uint32_t index, VkDescriptorSet set) {
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                            index, 1, &set, 0, nullptr);
}

void RenderPassEncoder::setPushConstants(const void* data, uint32_t size, uint32_t offset) {
    vkCmdPushConstants(cmd_, layout_, pushStages_, offset, size, data);
}

void RenderPassEncoder::setViewport(float x, float y, float width, float height,
                                    float minDepth, float maxDepth) {
    VkViewport viewport{x, y, width, height, minDepth, maxDepth};
    vkCmdSetViewport(cmd_, 0, 1, &viewport);
}

void RenderPassEncoder::setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    VkRect2D scissor{{x, y}, {width, height}};
    vkCmdSetScissor(cmd_, 0, 1, &scissor);
}

void RenderPassEncoder::setVertexBuffer(const saida::Buffer& buffer, uint64_t offset) {
    VkBuffer handle = buffer.handle();
    VkDeviceSize vkOffset = offset;
    vkCmdBindVertexBuffers(cmd_, 0, 1, &handle, &vkOffset);
}

void RenderPassEncoder::setIndexBuffer(const saida::Buffer& buffer, rhi::IndexType type,
                                       uint64_t offset) {
    vkCmdBindIndexBuffer(cmd_, buffer.handle(), offset, toVk(type));
}

void RenderPassEncoder::draw(uint32_t vertexCount, uint32_t instanceCount,
                             uint32_t firstVertex, uint32_t firstInstance) {
    vkCmdDraw(cmd_, vertexCount, instanceCount, firstVertex, firstInstance);
}

void RenderPassEncoder::drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                                    uint32_t firstIndex, int32_t vertexOffset,
                                    uint32_t firstInstance) {
    vkCmdDrawIndexed(cmd_, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void RenderPassEncoder::drawIndirect(const saida::Buffer& buffer, uint64_t offset,
                                     uint32_t drawCount, uint32_t stride) {
    vkCmdDrawIndirect(cmd_, buffer.handle(), offset, drawCount, stride);
}

void RenderPassEncoder::drawIndexedIndirect(const saida::Buffer& buffer, uint64_t offset,
                                            uint32_t drawCount, uint32_t stride) {
    vkCmdDrawIndexedIndirect(cmd_, buffer.handle(), offset, drawCount, stride);
}

void RenderPassEncoder::drawIndexedIndirectCount(const saida::Buffer& args, uint64_t argsOffset,
                                                 const saida::Buffer& count, uint64_t countOffset,
                                                 uint32_t maxDrawCount, uint32_t stride) {
    // Core Vulkan 1.2 symbol; the engine's device is 1.3 and callers guard on
    // Capabilities::drawIndirectCount.
    vkCmdDrawIndexedIndirectCount(cmd_, args.handle(), argsOffset, count.handle(), countOffset,
                                  maxDrawCount, stride);
}

void RenderPassEncoder::end() {
    vkCmdEndRendering(cmd_);
}

// ---- ComputePassEncoder ----

void ComputePassEncoder::setPipeline(const saida::ComputePipeline& pipeline) {
    pipeline.bind(cmd_);
    layout_ = pipeline.layout();
}

void ComputePassEncoder::setBindGroup(uint32_t index, const BindGroup& group) {
    setBindGroup(index, group.handle());
}

void ComputePassEncoder::setBindGroup(uint32_t index, VkDescriptorSet set) {
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, layout_,
                            index, 1, &set, 0, nullptr);
}

void ComputePassEncoder::setPushConstants(const void* data, uint32_t size, uint32_t offset) {
    vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_COMPUTE_BIT, offset, size, data);
}

void ComputePassEncoder::dispatch(uint32_t x, uint32_t y, uint32_t z) {
    vkCmdDispatch(cmd_, x, y, z);
}

// ---- CommandEncoder ----

void CommandEncoder::transition(VkImage image, rhi::ResourceState from, rhi::ResourceState to,
                                uint32_t baseLayer, uint32_t layerCount, bool discardContents,
                                rhi::TextureAspect aspect) {
    const StateInfo src = stateInfo(from);
    const StateInfo dst = stateInfo(to);

    auto isDepthState = [](rhi::ResourceState s) {
        return s == rhi::ResourceState::DepthWrite || s == rhi::ResourceState::DepthRead;
    };
    VkImageAspectFlags vkAspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (aspect == rhi::TextureAspect::Depth ||
        (aspect == rhi::TextureAspect::Auto && (isDepthState(from) || isDepthState(to))))
        vkAspect = VK_IMAGE_ASPECT_DEPTH_BIT;

    VkImageMemoryBarrier2 barrier = imageBarrier2(
        image,
        discardContents ? VK_IMAGE_LAYOUT_UNDEFINED : src.layout, dst.layout,
        src.stages, src.access, dst.stages, dst.access,
        vkAspect, baseLayer, layerCount);
    // imageBarrier2 covers one mip level by default; transitions apply to all.
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    cmdImageBarrier(cmd_, barrier);
}

void CommandEncoder::storageBarrier() {
    cmdComputeToComputeBarrier(cmd_);
}

namespace {
void memoryBarrier2(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = srcStage;
    mb.srcAccessMask = srcAccess;
    mb.dstStageMask = dstStage;
    mb.dstAccessMask = dstAccess;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}
} // namespace

void CommandEncoder::computeToGraphicsBarrier() {
    memoryBarrier2(cmd_,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
}

void CommandEncoder::computeToIndirectBarrier() {
    memoryBarrier2(cmd_,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
        VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
}

void CommandEncoder::transferToComputeBarrier() {
    memoryBarrier2(cmd_,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
}

void CommandEncoder::fillBuffer(const saida::Buffer& dst, uint64_t offset, uint64_t size,
                                uint32_t value) {
    vkCmdFillBuffer(cmd_, dst.handle(), offset, size, value);
}

RenderPassEncoder CommandEncoder::beginRenderPass(const RenderPassDesc& desc) {
    std::array<VkRenderingAttachmentInfo, 4> colorAttachments{};
    for (uint32_t i = 0; i < desc.colorCount; ++i) {
        const ColorAttachment& src = desc.colors[i];
        VkRenderingAttachmentInfo& attach = colorAttachments[i];
        attach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        attach.imageView = src.view;
        attach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attach.loadOp = toVk(src.loadOp);
        attach.storeOp = src.store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attach.clearValue.color = {{src.clearColor[0], src.clearColor[1],
                                    src.clearColor[2], src.clearColor[3]}};
        if (src.resolveView) {
            attach.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            attach.resolveImageView = src.resolveView;
            attach.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }

    VkRenderingAttachmentInfo depthAttach{};
    if (desc.depth.view) {
        depthAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttach.imageView = desc.depth.view;
        depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttach.loadOp = toVk(desc.depth.loadOp);
        depthAttach.storeOp = desc.depth.store ? VK_ATTACHMENT_STORE_OP_STORE
                                               : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttach.clearValue.depthStencil = {desc.depth.clearDepth, 0};
        if (desc.depth.resolveView) {
            depthAttach.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            depthAttach.resolveImageView = desc.depth.resolveView;
            depthAttach.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        }
    }

    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea.offset = {desc.x, desc.y};
    info.renderArea.extent = {desc.width, desc.height};
    info.layerCount = desc.layerCount;
    info.viewMask = desc.viewMask;
    info.colorAttachmentCount = desc.colorCount;
    info.pColorAttachments = desc.colorCount > 0 ? colorAttachments.data() : nullptr;
    info.pDepthAttachment = desc.depth.view ? &depthAttach : nullptr;
    vkCmdBeginRendering(cmd_, &info);

    RenderPassEncoder pass(cmd_);
    if (desc.defaultViewportScissor) {
        pass.setViewport(static_cast<float>(desc.x), static_cast<float>(desc.y),
                         static_cast<float>(desc.width), static_cast<float>(desc.height));
        pass.setScissor(desc.x, desc.y, desc.width, desc.height);
    }
    return pass;
}

void CommandEncoder::copyBufferToBuffer(const saida::Buffer& src, const saida::Buffer& dst,
                                        uint64_t size, uint64_t srcOffset, uint64_t dstOffset) {
    VkBufferCopy region{};
    region.srcOffset = srcOffset;
    region.dstOffset = dstOffset;
    region.size = size;
    vkCmdCopyBuffer(cmd_, src.handle(), dst.handle(), 1, &region);
}

void CommandEncoder::clearColorTexture(VkImage image, const std::array<float, 4>& color) {
    VkClearColorValue value{};
    value.float32[0] = color[0];
    value.float32[1] = color[1];
    value.float32[2] = color[2];
    value.float32[3] = color[3];
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS,
                                  0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdClearColorImage(cmd_, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
}

void CommandEncoder::copyBufferToTexture(const saida::Buffer& src, VkImage dst,
                                         uint32_t width, uint32_t height,
                                         uint32_t mipLevel, uint32_t baseLayer,
                                         uint32_t layerCount, uint64_t srcOffset) {
    VkBufferImageCopy region{};
    region.bufferOffset = srcOffset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = mipLevel;
    region.imageSubresource.baseArrayLayer = baseLayer;
    region.imageSubresource.layerCount = layerCount;
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd_, src.handle(), dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);
}

} // namespace saida::rhi::vulkan
