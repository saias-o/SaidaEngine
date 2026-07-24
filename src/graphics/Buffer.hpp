#pragma once

#ifdef SAIDA_RHI_WEBGPU

#include "rhi/webgpu/Buffer.hpp"

namespace saida {
using Buffer = rhi::webgpu::Buffer;
using MemoryUsage = rhi::webgpu::MemoryUsage;
} // namespace saida

#else

#include "graphics/VmaFwd.hpp"
#include "rhi/BufferUsage.hpp"

#include <cstdint>

namespace saida {

class VulkanDevice;

// Where a buffer's memory lives.
enum class MemoryUsage {
    GpuOnly,      // device-local, not host-accessible (vertex/index buffers)
    HostVisible,  // host-accessible and persistently mapped (staging/uniforms)
};

// RAII wrapper around a VkBuffer backed by a VMA allocation. This is the Vulkan
// backend's rhi::Buffer (aliased in rhi/Rhi.hpp): its construction API is
// backend-neutral (rhi::BufferUsage, plain sizes); handle() exposes the Vulkan
// object for the backend's command recording.
class Buffer {
public:
    Buffer(VulkanDevice& device, uint64_t size, rhi::BufferUsage usage, MemoryUsage memory);
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    // Copies `size` bytes into the (HostVisible) buffer and flushes it.
    void write(const void* data, uint64_t size, uint64_t offset = 0);
    void flush(uint64_t size = UINT64_MAX, uint64_t offset = 0);
    VkBuffer handle() const { return buffer_; }
    uint64_t size() const { return size_; }
    void* mapped() const { return mapped_; }

private:
    VulkanDevice& device_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    uint64_t size_ = 0;
    void* mapped_ = nullptr;  // non-null for HostVisible buffers
    MemoryUsage memory_ = MemoryUsage::GpuOnly;
};

} // namespace saida

#endif // SAIDA_RHI_WEBGPU
