#pragma once

#include "ui/RmlUiRenderInterface.hpp"
#include <memory>

namespace saida {
class VulkanDevice;
class Texture;
namespace rhi::vulkan { class CommandEncoder; }

// RmlUi owns layout; this backend submits its immutable triangles to the GPU.
// The output keeps the existing straight-alpha sRGB WebCanvas texture contract.
class RmlUiGpuRenderer {
public:
    explicit RmlUiGpuRenderer(VulkanDevice& device);
    ~RmlUiGpuRenderer();
    RmlUiGpuRenderer(const RmlUiGpuRenderer&) = delete;
    RmlUiGpuRenderer& operator=(const RmlUiGpuRenderer&) = delete;
    void render(const RmlUiRenderInterface::DrawList& list,
                rhi::vulkan::CommandEncoder& encoder, Texture& target, uint32_t frameSlot);
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace saida
