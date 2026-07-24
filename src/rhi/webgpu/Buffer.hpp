#pragma once

#include "rhi/BufferUsage.hpp"
#include "rhi/webgpu/WebGpu.hpp"

#include <cstdint>
#include <vector>

// Host-visible buffers use a CPU shadow because WebGPU has no persistent mapping.

namespace saida::rhi::webgpu {

class Device;

enum class MemoryUsage { GpuOnly, HostVisible };

class Buffer {
public:
    Buffer(Device& device, uint64_t size, rhi::BufferUsage usage, MemoryUsage memory);
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    WGPUBuffer handle() const { return buffer_; }
    uint64_t size() const { return size_; }

    void* mapped() { return shadow_.empty() ? nullptr : shadow_.data(); }

    void write(const void* data, uint64_t size, uint64_t offset = 0);
    void flush(uint64_t size = 0);

private:
    Device& device_;
    WGPUBuffer buffer_ = nullptr;
    uint64_t size_ = 0;
    std::vector<uint8_t> shadow_;
};

} // namespace saida::rhi::webgpu
