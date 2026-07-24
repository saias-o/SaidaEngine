#pragma once

// Shared WebGPU helpers; this header is only built by the Emscripten target.

#include <webgpu/webgpu.h>

#include <cstdint>

namespace saida::rhi::webgpu {

inline WGPUStringView sv(const char* s) {
    WGPUStringView v;
    v.data = s;
    v.length = WGPU_STRLEN;
    return v;
}

// Push-constant emulation slots are dynamic-offset uniform slices; WebGPU
// requires 256-byte alignment for dynamic offsets.
inline constexpr uint32_t kPushSlotAlignment = 256;

} // namespace saida::rhi::webgpu
