#include "rhi/webgpu/Device.hpp"

#include "rhi/webgpu/CommandEncoder.hpp"

#include <cstdio>
#include <cstring>

namespace saida::rhi::webgpu {

namespace {

// requestAsync plumbing: the continuation must survive the two async hops.
struct PendingRequest {
    Device::ReadyFn onReady;
    WGPUInstance instance = nullptr;
};

void onDevice(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView,
              void* userdata, void*) {
    auto* pending = static_cast<PendingRequest*>(userdata);
    if (status != WGPURequestDeviceStatus_Success) {
        std::printf("rhi::webgpu: requestDevice failed\n");
        pending->onReady(nullptr);
        delete pending;
        return;
    }
    auto built = std::unique_ptr<Device>(new Device());
    built->initialize(pending->instance, device);
    pending->onReady(std::move(built));
    delete pending;
}

void onAdapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView,
               void* userdata, void*) {
    auto* pending = static_cast<PendingRequest*>(userdata);
    if (status != WGPURequestAdapterStatus_Success) {
        std::printf("rhi::webgpu: requestAdapter failed\n");
        pending->onReady(nullptr);
        delete pending;
        return;
    }
    WGPURequestDeviceCallbackInfo cb = {};
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    cb.callback = onDevice;
    cb.userdata1 = pending;
    wgpuAdapterRequestDevice(adapter, nullptr, cb);
}

} // namespace

void Device::requestAsync(ReadyFn onReady) {
    WGPUInstanceDescriptor idesc = {};
    WGPUInstance instance = wgpuCreateInstance(&idesc);

    auto* pending = new PendingRequest{std::move(onReady), instance};
    WGPURequestAdapterCallbackInfo cb = {};
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    cb.callback = onAdapter;
    cb.userdata1 = pending;
    wgpuInstanceRequestAdapter(instance, nullptr, cb);
}

void Device::initialize(WGPUInstance instance, WGPUDevice device) {
    instance_ = instance;
    device_ = device;
    queue_ = wgpuDeviceGetQueue(device_);

    // Vulkan-only capabilities stay false on this backend.
    capabilities_.apiVersion = 0;
    capabilities_.dynamicRendering = true;    // WebGPU renders without pass objects
    capabilities_.synchronization2 = false;   // sync is driver-managed
    capabilities_.timelineSemaphore = false;
    capabilities_.descriptorIndexing = false; // no bindless on today's WebGPU
    capabilities_.bufferDeviceAddress = false;
    capabilities_.drawIndirectCount = false;  // culled draws use instanceCount=0
    capabilities_.multiview = false;          // XR is out of the web scope
    capabilities_.multiDrawIndirect = false;
    capabilities_.rayQuery = false;
    capabilities_.dedicatedComputeQueue = false;
    capabilities_.discreteGpu = false;        // unknown; assume conservative
    capabilities_.maxSamples = 4;             // WebGPU guarantees 1 and 4
    capabilities_.tier = QualityTier::Low;

    WGPUBufferDescriptor ring = {};
    ring.label = sv("saida.pushRing");
    ring.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    ring.size = kPushRingBytes;
    pushRing_ = wgpuDeviceCreateBuffer(device_, &ring);
    pushStaging_.resize(kPushRingBytes);
}

WGPUBindGroupLayout Device::emptyBindGroupLayout() {
    if (!emptyLayout_) {
        WGPUBindGroupLayoutDescriptor desc = {};
        emptyLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &desc);
    }
    return emptyLayout_;
}

WGPUBindGroup Device::emptyBindGroup() {
    if (!emptyGroup_) {
        WGPUBindGroupDescriptor desc = {};
        desc.layout = emptyBindGroupLayout();
        emptyGroup_ = wgpuDeviceCreateBindGroup(device_, &desc);
    }
    return emptyGroup_;
}

WGPUBuffer Device::zeroBuffer(uint64_t size) {
    if (size > zeroBufferSize_) {
        if (zeroBuffer_) wgpuBufferRelease(zeroBuffer_);
        WGPUBufferDescriptor desc = {};
        desc.label = sv("saida.zeroBuffer");
        desc.usage = WGPUBufferUsage_CopySrc;  // never written: stays all-zero
        desc.size = size;
        zeroBuffer_ = wgpuDeviceCreateBuffer(device_, &desc);
        zeroBufferSize_ = size;
    }
    return zeroBuffer_;
}

Device::~Device() {
    if (zeroBuffer_) wgpuBufferRelease(zeroBuffer_);
    if (emptyGroup_) wgpuBindGroupRelease(emptyGroup_);
    if (emptyLayout_) wgpuBindGroupLayoutRelease(emptyLayout_);
    if (pushRing_) wgpuBufferRelease(pushRing_);
    if (queue_) wgpuQueueRelease(queue_);
    if (device_) wgpuDeviceRelease(device_);
    if (instance_) wgpuInstanceRelease(instance_);
}

uint32_t Device::allocPushSlot(const void* data, uint32_t size) {
    const uint32_t offset = pushCursor_;
    const uint32_t slot = (size + kPushSlotAlignment - 1) & ~(kPushSlotAlignment - 1);
    if (offset + slot > kPushRingBytes) {
        std::printf("rhi::webgpu: push ring exhausted (frame uses > %u bytes)\n", kPushRingBytes);
        return 0;
    }
    std::memcpy(pushStaging_.data() + offset, data, size);
    pushCursor_ = offset + slot;
    return offset;
}

void Device::flushPushRing() {
    if (pushCursor_ == pushFlushed_) return;
    // Upload only the slice recorded since the previous flush; writeBuffer is
    // ordered before any subsequently submitted command buffer.
    wgpuQueueWriteBuffer(queue_, pushRing_, pushFlushed_,
                         pushStaging_.data() + pushFlushed_, pushCursor_ - pushFlushed_);
    pushFlushed_ = pushCursor_;
}

void Device::withSingleTimeEncoder(const std::function<void(CommandEncoder&)>& fn) {
    CommandEncoder encoder(*this);
    fn(encoder);
    flushPushRing();
    WGPUCommandBuffer cmd = encoder.finish();
    wgpuQueueSubmit(queue_, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
}

} // namespace saida::rhi::webgpu
