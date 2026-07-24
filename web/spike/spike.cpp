// Spike 16.0 — WebGPU + GLFW canvas, animated clear. Validates the full web
// presentation path end to end: instance -> requestAdapter -> requestDevice
// (async callbacks) -> canvas surface -> per-frame render pass -> queue submit,
// driven by the browser's RAF via emscripten_set_main_loop. No render pipeline /
// WGSL yet — that path gets validated in 16.2 with the real transpiled shaders.
//
// Build: see CMakeLists.txt (emcmake) or build.sh.
#include <webgpu/webgpu.h>
#include <GLFW/glfw3.h>
#include <emscripten.h>

#include <cmath>
#include <cstdio>

namespace {

WGPUInstance gInstance = nullptr;
WGPUDevice gDevice = nullptr;
WGPUQueue gQueue = nullptr;
WGPUSurface gSurface = nullptr;
const WGPUTextureFormat kFormat = WGPUTextureFormat_BGRA8Unorm;
const uint32_t kWidth = 960, kHeight = 540;
double gTime = 0.0;

WGPUStringView sv(const char* s) {
    WGPUStringView v;
    v.data = s;
    v.length = WGPU_STRLEN;
    return v;
}

void frame() {
    gTime += 1.0 / 60.0;

    WGPUSurfaceTexture st = {};
    wgpuSurfaceGetCurrentTexture(gSurface, &st);
    if (!st.texture) return;
    WGPUTextureView view = wgpuTextureCreateView(st.texture, nullptr);

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gDevice, nullptr);

    WGPURenderPassColorAttachment color = {};
    color.view = view;
    color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    const double t = gTime;
    color.clearValue = {0.5 + 0.5 * std::sin(t),
                        0.5 + 0.5 * std::sin(t + 2.0),
                        0.5 + 0.5 * std::sin(t + 4.0), 1.0};

    WGPURenderPassDescriptor rp = {};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &color;

    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(gQueue, 1, &cmd);

    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    wgpuTextureViewRelease(view);
}

void onDevice(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView,
              void*, void*) {
    if (status != WGPURequestDeviceStatus_Success) {
        std::printf("spike: requestDevice failed\n");
        return;
    }
    gDevice = device;
    gQueue = wgpuDeviceGetQueue(device);

    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = {};
    canvas.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    canvas.selector = sv("#canvas");
    WGPUSurfaceDescriptor surfDesc = {};
    surfDesc.nextInChain = &canvas.chain;
    gSurface = wgpuInstanceCreateSurface(gInstance, &surfDesc);

    WGPUSurfaceConfiguration config = {};
    config.device = gDevice;
    config.format = kFormat;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = kWidth;
    config.height = kHeight;
    config.alphaMode = WGPUCompositeAlphaMode_Auto;
    config.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(gSurface, &config);

    std::printf("spike: device ready, surface configured — rendering\n");
    emscripten_set_main_loop(frame, 0, false);
}

void onAdapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView,
               void*, void*) {
    if (status != WGPURequestAdapterStatus_Success) {
        std::printf("spike: requestAdapter failed\n");
        return;
    }
    WGPURequestDeviceCallbackInfo cb = {};
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    cb.callback = onDevice;
    wgpuAdapterRequestDevice(adapter, nullptr, cb);
}

} // namespace

int main() {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // WebGPU, not WebGL
    glfwCreateWindow(int(kWidth), int(kHeight), "Saida web spike", nullptr, nullptr);

    WGPUInstanceDescriptor idesc = {};
    gInstance = wgpuCreateInstance(&idesc);

    WGPURequestAdapterCallbackInfo cb = {};
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    cb.callback = onAdapter;
    wgpuInstanceRequestAdapter(gInstance, nullptr, cb);

    // Return to the browser but keep the runtime alive so the async adapter/device
    // callbacks (and then the main loop) can run.
    emscripten_exit_with_live_runtime();
    return 0;
}
