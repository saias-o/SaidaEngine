#include "rhi/webgpu/Sampler.hpp"

#include "rhi/webgpu/Device.hpp"

#include <stdexcept>

namespace saida::rhi::webgpu {

namespace {

WGPUFilterMode toWgpu(rhi::FilterMode mode) {
    return mode == rhi::FilterMode::Linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
}

WGPUAddressMode toWgpu(rhi::AddressMode mode) {
    switch (mode) {
        case rhi::AddressMode::Repeat:        return WGPUAddressMode_Repeat;
        case rhi::AddressMode::ClampToEdge:   return WGPUAddressMode_ClampToEdge;
        // WebGPU has no border colour; clamp-to-edge is the closest behaviour.
        // The only border user is the shadow sampler ("outside = lit"), where
        // edge-clamped depth still resolves as lit for the common case.
        case rhi::AddressMode::ClampToBorder: return WGPUAddressMode_ClampToEdge;
    }
    return WGPUAddressMode_ClampToEdge;
}

WGPUCompareFunction toWgpu(rhi::CompareOp op) {
    switch (op) {
        case rhi::CompareOp::Never:          return WGPUCompareFunction_Never;
        case rhi::CompareOp::Less:           return WGPUCompareFunction_Less;
        case rhi::CompareOp::Equal:          return WGPUCompareFunction_Equal;
        case rhi::CompareOp::LessOrEqual:    return WGPUCompareFunction_LessEqual;
        case rhi::CompareOp::Greater:        return WGPUCompareFunction_Greater;
        case rhi::CompareOp::NotEqual:       return WGPUCompareFunction_NotEqual;
        case rhi::CompareOp::GreaterOrEqual: return WGPUCompareFunction_GreaterEqual;
        case rhi::CompareOp::Always:         return WGPUCompareFunction_Always;
    }
    return WGPUCompareFunction_Less;
}

} // namespace

Sampler::Sampler(Device& device, const rhi::SamplerDesc& desc) {
    WGPUSamplerDescriptor sd = {};
    sd.addressModeU = toWgpu(desc.addressMode);
    sd.addressModeV = toWgpu(desc.addressMode);
    sd.addressModeW = toWgpu(desc.addressMode);
    sd.magFilter = toWgpu(desc.magFilter);
    sd.minFilter = toWgpu(desc.minFilter);
    sd.mipmapFilter = desc.mipFilter == rhi::FilterMode::Linear
                          ? WGPUMipmapFilterMode_Linear
                          : WGPUMipmapFilterMode_Nearest;
    sd.lodMaxClamp = 32.0f;
    sd.maxAnisotropy = 1;
    if (desc.compareEnabled) sd.compare = toWgpu(desc.compare);
    sampler_ = wgpuDeviceCreateSampler(device.device(), &sd);
    if (!sampler_) throw std::runtime_error("webgpu: failed to create sampler");
}

Sampler::~Sampler() {
    if (sampler_) wgpuSamplerRelease(sampler_);
}

} // namespace saida::rhi::webgpu
