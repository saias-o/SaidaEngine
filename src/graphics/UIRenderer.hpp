#pragma once

#include "rhi/Rhi.hpp"
#include "ui/HudRasterizer.hpp"

#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace saida {

class ResourceManager;
class Scene;
class UINode;
class WebCanvasNode;
class UICanvasNode;
#ifndef SAIDA_RHI_WEBGPU
// Desktop HUD texture type; on WebGPU `saida::Texture` is a type alias
// (graphics/Texture.hpp), so forward-declaring it there would clash.
class Texture;
#endif

struct UIDrawCmd {
    glm::vec2 position;
    glm::vec2 size;
    glm::vec2 corners[4];
    glm::vec4 color;
    uint32_t textureId;
    uint32_t hasTexture;
    uint32_t useCorners = 0;
    int sortOrder = 0;
#ifdef SAIDA_RHI_WEBGPU
    rhi::BindGroup* textureGroup = nullptr;
#endif
};

class UIRenderer {
public:
    UIRenderer(rhi::Device& device, ResourceManager& resources, rhi::Format colorFormat);
    ~UIRenderer();
    UIRenderer(const UIRenderer&) = delete;
    UIRenderer& operator=(const UIRenderer&) = delete;

    void gatherUI(Scene& scene, glm::vec2 viewportSize = glm::vec2(0.0f));
    void updateAsyncTextures(rhi::CommandEncoder& encoder);
    void recordCommands(rhi::RenderPassEncoder& rp, uint32_t width, uint32_t height,
                        glm::vec2 viewportOffset = glm::vec2(0.0f),
                        glm::vec2 viewportSize = glm::vec2(0.0f));

private:
#ifndef SAIDA_RHI_WEBGPU
    void traverseUI(UINode* node);
#endif
    // Rasterize the UICanvas text HUD via the shared CPU RmlUi path and push a
    // fullscreen composite draw. Runs on desktop and Web so both show identical
    // HUD text (visual-parity invariant); the texture upload is the only
    // platform-specific step.
    void gatherHud(UICanvasNode& canvas, glm::vec2 viewportSize);

    rhi::Device& device_;
    ResourceManager& resources_;

    std::unique_ptr<rhi::Pipeline> pipeline_;

    std::vector<UIDrawCmd> drawCmds_;
    std::vector<WebCanvasNode*> webNodesToUpdate_;
    bool loggedWebCanvasGather_ = false;

    HudRasterizer hud_;
#ifdef SAIDA_RHI_WEBGPU
    std::unique_ptr<rhi::BindGroupLayout> textureLayout_;
    std::unique_ptr<rhi::Texture> hudTexture_;
    std::unique_ptr<rhi::BindGroup> hudTextureGroup_;
#else
    std::unique_ptr<Texture> hudTexture_;
#endif
};

} // namespace saida
