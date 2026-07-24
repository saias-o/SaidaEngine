#include "rhi/webgpu/Buffer.hpp"

#include "rhi/webgpu/Device.hpp"

#include <algorithm>
#include <cstring>

namespace saida::rhi::webgpu {

namespace {

WGPUBufferUsage toWgpu(rhi::BufferUsage usage) {
    WGPUBufferUsage flags = WGPUBufferUsage_None;
    if (hasFlag(usage, rhi::BufferUsage::TransferSrc)) flags |= WGPUBufferUsage_CopySrc;
    if (hasFlag(usage, rhi::BufferUsage::TransferDst)) flags |= WGPUBufferUsage_CopyDst;
    if (hasFlag(usage, rhi::BufferUsage::Uniform)) flags |= WGPUBufferUsage_Uniform;
    if (hasFlag(usage, rhi::BufferUsage::Storage)) flags |= WGPUBufferUsage_Storage;
    if (hasFlag(usage, rhi::BufferUsage::Index)) flags |= WGPUBufferUsage_Index;
    if (hasFlag(usage, rhi::BufferUsage::Vertex)) flags |= WGPUBufferUsage_Vertex;
    if (hasFlag(usage, rhi::BufferUsage::Indirect)) flags |= WGPUBufferUsage_Indirect;
    return flags;
}

} // namespace

Buffer::Buffer(Device& device, uint64_t size, rhi::BufferUsage usage, MemoryUsage memory)
    : device_(device), size_(size) {
    WGPUBufferDescriptor desc = {};
    // HostVisible buffers upload through queue.writeBuffer -> need CopyDst.
    desc.usage = toWgpu(usage) |
                 (memory == MemoryUsage::HostVisible ? WGPUBufferUsage_CopyDst
                                                     : WGPUBufferUsage_None);
    desc.size = (size + 3) & ~uint64_t(3);  // writeBuffer requires 4-byte multiples
    buffer_ = wgpuDeviceCreateBuffer(device_.device(), &desc);

    if (memory == MemoryUsage::HostVisible) shadow_.resize(desc.size, 0);
}

Buffer::~Buffer() {
    if (buffer_) wgpuBufferRelease(buffer_);
}

void Buffer::write(const void* data, uint64_t size, uint64_t offset) {
    if (!shadow_.empty()) {
        std::memcpy(shadow_.data() + offset, data, size);
        // Mirror the Vulkan persistent-mapped behaviour: write() is visible
        // immediately (coherent memory) -> upload right away.
        const uint64_t aligned = (size + 3) & ~uint64_t(3);
        wgpuQueueWriteBuffer(device_.queue(), buffer_, offset,
                             shadow_.data() + offset, std::min(aligned, shadow_.size() - offset));
    } else {
        wgpuQueueWriteBuffer(device_.queue(), buffer_, offset, data, (size + 3) & ~uint64_t(3));
    }
}

void Buffer::flush(uint64_t size) {
    if (shadow_.empty()) return;
    const uint64_t bytes = size == 0 ? shadow_.size() : std::min((size + 3) & ~uint64_t(3),
                                                                 uint64_t(shadow_.size()));
    wgpuQueueWriteBuffer(device_.queue(), buffer_, 0, shadow_.data(), bytes);
}

} // namespace saida::rhi::webgpu
