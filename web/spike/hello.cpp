// Spike 16.0 — step A: prove the Emscripten toolchain compiles C++ to wasm and
// runs. No WebGPU yet; this just validates emcc + the wasm runtime end to end.
#include <cstdio>

int main() {
    std::printf("SaidaEngine web spike: wasm runtime OK\n");
    return 0;
}
