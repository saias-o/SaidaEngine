#pragma once

// Vulkan-only: reading the presented image back has no WebGPU equivalent here,
// and under SAIDA_RHI_WEBGPU `saida::Buffer` is a type alias rather than a
// class, so even the forward declaration below would not compile.
#ifndef SAIDA_RHI_WEBGPU

#include <cstdint>
#include <memory>
#include <string>

namespace saida {

class Buffer;
class Swapchain;
class VulkanDevice;

namespace rhi::vulkan {
class CommandEncoder;
}

// Reads the presented frame back to the host and writes it as a PNG.
//
// Invariant: at most one capture is pending at a time, and a pending capture
// owns a staging buffer sized for exactly one swapchain image. The buffer is
// allocated when a capture is requested and released once it is written, so a
// process that never captures pays nothing.
//
// This is the composite the player actually sees — 3D and UI together — which
// nothing outside a human looking at a screen could verify before. It is the
// capability ROADMAP section 3 declares an absolute prerequisite to
// decomposing the Renderer, and the basis of a golden-image net.
//
// Ownership follows the rule the Renderer's own decomposition must obey: this
// unit owns its GPU objects and exposes only record/resolve. It never reads
// another pass's descriptors and never sequences itself.
class FrameCapture {
public:
    FrameCapture(VulkanDevice& device, Swapchain& swapchain);
    ~FrameCapture();

    FrameCapture(const FrameCapture&) = delete;
    FrameCapture& operator=(const FrameCapture&) = delete;

    // Ask for the next recorded frame to be captured to `pngPath`. Returns
    // false, with `error` filled, when the surface does not allow reading the
    // presented image back or the staging buffer cannot be allocated — the
    // caller must report that rather than continue silently.
    bool request(const std::string& pngPath, std::string& error);

    bool pending() const { return pending_; }

    // Record the copy from the presented image into the staging buffer. Called
    // by the Renderer while the swapchain image is still in ColorAttachment,
    // just before it transitions to Present; does nothing when no capture is
    // pending. The image is left in ColorAttachment.
    void recordCopy(rhi::vulkan::CommandEncoder& encoder, uint32_t imageIndex);

    // Convert the staged pixels and write the PNG. The caller must have waited
    // for the recorded copy to complete. Returns false with `error` set on a
    // conversion or I/O failure. Clears the pending state either way.
    bool resolve(std::string& error);

private:
    VulkanDevice& device_;
    Swapchain& swapchain_;

    std::unique_ptr<Buffer> staging_;
    std::string pngPath_;
    bool pending_ = false;
    bool copied_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

} // namespace saida

#endif // SAIDA_RHI_WEBGPU
