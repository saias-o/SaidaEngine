#include "graphics/Buffer.hpp"

#include "graphics/MemoryProfiler.hpp"
#include "graphics/VulkanDevice.hpp"
#include "vk_mem_alloc.h"

#include <cstring>
#include <stdexcept>

namespace saida {

// Maps the backend-neutral rhi::BufferUsage flags onto Vulkan usage bits.
static VkBufferUsageFlags toVkBufferUsage(rhi::BufferUsage u) {
    using U = rhi::BufferUsage;
    VkBufferUsageFlags f = 0;
    if (rhi::hasFlag(u, U::Vertex))      f |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (rhi::hasFlag(u, U::Index))       f |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (rhi::hasFlag(u, U::Uniform))     f |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (rhi::hasFlag(u, U::Storage))     f |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (rhi::hasFlag(u, U::Indirect))    f |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (rhi::hasFlag(u, U::TransferSrc)) f |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (rhi::hasFlag(u, U::TransferDst)) f |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return f;
}

Buffer::Buffer(VulkanDevice& device, uint64_t size, rhi::BufferUsage usage, MemoryUsage memory)
    : device_(device), size_(size), memory_(memory) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = toVkBufferUsage(usage);
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    if (memory == MemoryUsage::HostVisible) {
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                        | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    VmaAllocationInfo result{};
    if (vmaCreateBuffer(device_.allocator(), &bufferInfo, &allocInfo,
                        &buffer_, &allocation_, &result) != VK_SUCCESS)
        throw std::runtime_error("failed to create buffer");

    mapped_ = result.pMappedData;  // null when not host-visible
    MemoryProfiler::registerAllocation(
        memory_ == MemoryUsage::HostVisible ? "Buffer/HostVisible" : "Buffer/GpuOnly",
        static_cast<uint64_t>(size_));
}

Buffer::~Buffer() {
    MemoryProfiler::unregisterAllocation(
        memory_ == MemoryUsage::HostVisible ? "Buffer/HostVisible" : "Buffer/GpuOnly",
        static_cast<uint64_t>(size_));
    vmaDestroyBuffer(device_.allocator(), buffer_, allocation_);
}

void Buffer::write(const void* data, uint64_t size, uint64_t offset) {
    if (!mapped_)
        throw std::runtime_error("write() called on a non host-visible buffer");
    memcpy(static_cast<uint8_t*>(mapped_) + offset, data, static_cast<size_t>(size));
    vmaFlushAllocation(device_.allocator(), allocation_, 0, size);
}

void Buffer::flush(uint64_t size, uint64_t offset) {
    if (!mapped_) return;
    // UINT64_MAX == VK_WHOLE_SIZE, so the default forwards straight through.
    vmaFlushAllocation(device_.allocator(), allocation_, offset, size);
}

} // namespace saida
