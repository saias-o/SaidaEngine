// Spike 16.0 — step B: prove the emdawnwebgpu WebGPU port is available and its
// header compiles against this Emscripten. Just references a few types/entry
// points so the compile fails loudly if the API isn't what we expect.
#include <webgpu/webgpu.h>
#include <cstdio>

int main() {
    WGPUInstanceDescriptor desc = {};
    WGPUInstance instance = wgpuCreateInstance(&desc);
    std::printf("instance=%p\n", (void*)instance);
    return instance ? 0 : 1;
}
