#pragma once

#include "rhi/Capabilities.hpp"
#include "rhi/webgpu/WebGpu.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

// WebGPU device creation is asynchronous.
// WGSL push constants use 256-byte-aligned dynamic uniform slices.

namespace saida::rhi::webgpu {

class CommandEncoder;

class Device {
public:
    using ReadyFn = std::function<void(std::unique_ptr<Device>)>;

    // The runtime must stay alive until this callback runs.
    static void requestAsync(ReadyFn onReady);

    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    WGPUInstance instance() const { return instance_; }
    WGPUDevice device() const { return device_; }
    WGPUQueue queue() const { return queue_; }

    const rhi::Capabilities& capabilities() const { return capabilities_; }

    // One-shot command batch (staging/init). WebGPU has no fences to wait on;
    // the submit is ordered before any later frame submit, which is all the
    // callers rely on.
    void withSingleTimeEncoder(const std::function<void(CommandEncoder&)>& fn);

    void waitIdle() const {}  // driver-managed on web

    // RAF serializes frames, so the previous submit has consumed the ring.
    void beginFrame() { pushCursor_ = 0; pushFlushed_ = 0; }
    uint32_t allocPushSlot(const void* data, uint32_t size);
    void flushPushRing();
    WGPUBuffer pushRingBuffer() const { return pushRing_; }

    // WebGPU pipeline layouts require contiguous bind-group indices.
    WGPUBindGroupLayout emptyBindGroupLayout();
    WGPUBindGroup emptyBindGroup();

    // WebGPU has no clear-texture command outside a render pass.
    WGPUBuffer zeroBuffer(uint64_t size);

    Device() = default;
    void initialize(WGPUInstance instance, WGPUDevice device);

private:

    WGPUInstance instance_ = nullptr;
    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    rhi::Capabilities capabilities_{};

    static constexpr uint32_t kPushRingBytes = 256 * 1024;  // 1024 slots/frame
    WGPUBindGroupLayout emptyLayout_ = nullptr;
    WGPUBindGroup emptyGroup_ = nullptr;
    WGPUBuffer pushRing_ = nullptr;
    std::vector<uint8_t> pushStaging_;
    uint32_t pushCursor_ = 0;
    uint32_t pushFlushed_ = 0;
    WGPUBuffer zeroBuffer_ = nullptr;
    uint64_t zeroBufferSize_ = 0;
};

} // namespace saida::rhi::webgpu
