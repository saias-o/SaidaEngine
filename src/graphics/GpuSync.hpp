#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <stdexcept>
#include <utility>

// Modern synchronization helpers (Vulkan 1.3 synchronization2 + 1.2 timeline
// semaphores). These compile against the 1.3 headers unconditionally; the
// runtime calls are only valid when the device enabled the matching features
// (VulkanDevice::capabilities()). They keep the upcoming async-compute GI passes
// readable instead of re-rolling barrier boilerplate everywhere.

namespace saida {

// Build a synchronization2 image barrier (does not record it).
inline VkImageMemoryBarrier2 imageBarrier2(
    VkImage image,
    VkImageLayout oldLayout, VkImageLayout newLayout,
    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
    uint32_t baseLayer = 0, uint32_t layerCount = 1) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, baseLayer, layerCount};
    return b;
}

// Record one or more image barriers via vkCmdPipelineBarrier2.
inline void cmdImageBarriers(VkCommandBuffer cmd, const VkImageMemoryBarrier2* barriers,
                             uint32_t count) {
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = count;
    dep.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(cmd, &dep);
}

inline void cmdImageBarrier(VkCommandBuffer cmd, const VkImageMemoryBarrier2& barrier) {
    cmdImageBarriers(cmd, &barrier, 1);
}

// Global memory barrier between two compute dispatches reading/writing storage
// resources (the common case for cascade/cache update chains).
inline void cmdComputeToComputeBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// RAII timeline semaphore: a monotonically increasing counter used to sync the
// graphics and async-compute queues without per-frame fences.
class TimelineSemaphore {
public:
    TimelineSemaphore() = default;
    TimelineSemaphore(VkDevice device, uint64_t initialValue) : device_(device) {
        VkSemaphoreTypeCreateInfo type{};
        type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue = initialValue;
        VkSemaphoreCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        ci.pNext = &type;
        if (vkCreateSemaphore(device_, &ci, nullptr, &handle_) != VK_SUCCESS)
            throw std::runtime_error("failed to create timeline semaphore");
    }
    ~TimelineSemaphore() {
        if (handle_) vkDestroySemaphore(device_, handle_, nullptr);
    }
    TimelineSemaphore(const TimelineSemaphore&) = delete;
    TimelineSemaphore& operator=(const TimelineSemaphore&) = delete;
    TimelineSemaphore(TimelineSemaphore&& o) noexcept { *this = std::move(o); }
    TimelineSemaphore& operator=(TimelineSemaphore&& o) noexcept {
        if (this != &o) {
            if (handle_) vkDestroySemaphore(device_, handle_, nullptr);
            device_ = o.device_; handle_ = o.handle_;
            o.handle_ = VK_NULL_HANDLE;
        }
        return *this;
    }

    VkSemaphore handle() const { return handle_; }

    // Block until the counter reaches `value`.
    void wait(uint64_t value, uint64_t timeout = UINT64_MAX) const {
        VkSemaphoreWaitInfo wi{};
        wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wi.semaphoreCount = 1;
        wi.pSemaphores = &handle_;
        wi.pValues = &value;
        vkWaitSemaphores(device_, &wi, timeout);
    }

    uint64_t value() const {
        uint64_t v = 0;
        vkGetSemaphoreCounterValue(device_, handle_, &v);
        return v;
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkSemaphore handle_ = VK_NULL_HANDLE;
};

} // namespace saida
