#include "graphics/UIRenderer.hpp"
#include "core/Profiler.hpp"
#include "graphics/ResourceManager.hpp"
#include "graphics/Pipeline.hpp"
#include "scene/Scene.hpp"
#include "nodes/UICanvasNode.hpp"
#ifndef SAIDA_RHI_WEBGPU
#include "graphics/Texture.hpp"
#include "nodes/UIColorNode.hpp"
#include "nodes/UIImageNode.hpp"
#include "nodes/UIButtonNode.hpp"
#include "nodes/UIToggleNode.hpp"
#include "nodes/WebCanvasNode.hpp"
#endif
#include "core/Log.hpp"
#include "core/Paths.hpp"

#include <algorithm>
#include <cmath>

namespace saida {

// The HUD composite draws over every other UI quad; text belongs on top of any
// background panels rasterized by traverseUI.
constexpr int kHudSortOrder = 1 << 20;

struct UIPushConstants {
    glm::vec2 position;
    glm::vec2 size;
    glm::vec2 screenSize;
    uint32_t textureId;
    uint32_t hasTexture;
    glm::vec4 color;
    glm::vec2 corners[4];
    uint32_t useCorners;
    uint32_t _pad;
};

UIRenderer::UIRenderer(rhi::Device& device, ResourceManager& resources, rhi::Format colorFormat)
    : device_(device), resources_(resources) {
    Pipeline::Desc desc;
    desc.vertPath = shaderPath("ui.vert.spv");
    desc.fragPath = shaderPath("ui.frag.spv");
    desc.colorFormats = {colorFormat};
#ifdef SAIDA_RHI_WEBGPU
    textureLayout_ = std::make_unique<rhi::BindGroupLayout>(device_,
        std::vector<rhi::webgpu::BindGroupLayoutEntry>{
            {0, rhi::BindingType::SampledTexture, rhi::ShaderStages::Fragment},
            {1, rhi::BindingType::Sampler, rhi::ShaderStages::Fragment},
        });
    desc.bindGroupLayouts = {textureLayout_.get()};
#else
    desc.bindGroupLayouts = {resources_.globalMaterialSetLayout()};  // raw bindless set
#endif
    desc.vertexInput = false;
    desc.depthTest = false;
    desc.depthWrite = false;
    desc.cullMode = rhi::CullMode::None;
    desc.blendMode = rhi::BlendMode::Alpha;
    desc.topology = rhi::Topology::TriangleStrip;
    desc.pushConstantSize = sizeof(UIPushConstants);
    pipeline_ = std::make_unique<Pipeline>(device_, desc);

    Log::info("UIRenderer: pipeline created");
}

UIRenderer::~UIRenderer() = default;

void UIRenderer::gatherHud(UICanvasNode& canvas, glm::vec2 viewportSize) {
    if (viewportSize.x <= 0.0f || viewportSize.y <= 0.0f) return;
    const HudRasterizer::Frame frame = hud_.rasterize(canvas, viewportSize);
    if (!frame.hasContent) return;  // no text: nothing to composite

    const uint32_t width = frame.width;
    const uint32_t height = frame.height;

#ifdef SAIDA_RHI_WEBGPU
    if (frame.changed && frame.pixels) {
        if (!hudTexture_ || hudTexture_->width() != width || hudTexture_->height() != height) {
            hudTexture_ = std::make_unique<rhi::Texture>(
                device_, frame.pixels->data(), width, height, rhi::Format::RGBA8Unorm, false);
            hudTextureGroup_.reset();
        } else {
            hudTexture_->updatePixels(frame.pixels->data(), frame.pixels->size());
        }
        if (!hudTextureGroup_) {
            rhi::BindGroupEntry textureEntry;
            textureEntry.binding = 0;
            textureEntry.view = hudTexture_->imageView();
            rhi::BindGroupEntry samplerEntry;
            samplerEntry.binding = 1;
            samplerEntry.sampler = hudTexture_->sampler();
            hudTextureGroup_ = std::make_unique<rhi::BindGroup>(
                *textureLayout_, std::vector<rhi::BindGroupEntry>{textureEntry, samplerEntry});
        }
    }
    if (!hudTextureGroup_) return;

    UIDrawCmd cmd{};
    cmd.size = {static_cast<float>(width), static_cast<float>(height)};
    cmd.color = glm::vec4(1.0f);
    cmd.hasTexture = 1;
    cmd.sortOrder = kHudSortOrder;
    cmd.textureGroup = hudTextureGroup_.get();
    drawCmds_.push_back(cmd);
#else
    if (frame.changed && frame.pixels) {
        if (!hudTexture_ || hudTexture_->width() != width || hudTexture_->height() != height) {
            hudTexture_ = std::make_unique<Texture>(
                resources_.device(), frame.pixels->data(), width, height, rhi::Format::RGBA8Unorm, false);
        } else {
            hudTexture_->updatePixels(frame.pixels->data(), frame.pixels->size());
        }
    }
    if (!hudTexture_) return;

    UIDrawCmd cmd{};
    cmd.size = {static_cast<float>(width), static_cast<float>(height)};
    cmd.color = glm::vec4(1.0f);
    cmd.hasTexture = 1;
    cmd.sortOrder = kHudSortOrder;
    cmd.textureId = resources_.ensureBindlessTextureIndex(hudTexture_.get());
    drawCmds_.push_back(cmd);
#endif
}

void UIRenderer::gatherUI(Scene& scene, glm::vec2 viewportSize) {
    SAIDA_PROFILE_FUNCTION();
    drawCmds_.clear();
    webNodesToUpdate_.clear();
    
    UICanvasNode* canvas = scene.uiCanvas();
#ifndef SAIDA_RHI_WEBGPU
    for (WebCanvasNode* wcn : scene.webCanvases()) {
        if (!wcn || !wcn->isActiveInHierarchy()) continue;
        if (wcn->mode() == WebCanvasNode::Mode::ScreenSpace) {
            glm::vec2 pos = wcn->screenPosition();
            glm::vec2 size = wcn->screenSize();
            bool fillsViewport = viewportSize.x > 0.0f && viewportSize.y > 0.0f &&
                std::abs(pos.x) < 0.5f && std::abs(pos.y) < 0.5f &&
                std::abs(size.x - static_cast<float>(wcn->width())) < 0.5f &&
                std::abs(size.y - static_cast<float>(wcn->height())) < 0.5f;
            if (fillsViewport) {
                uint32_t targetWidth = std::max(1u, static_cast<uint32_t>(std::round(viewportSize.x)));
                uint32_t targetHeight = std::max(1u, static_cast<uint32_t>(std::round(viewportSize.y)));
                if (wcn->width() != targetWidth || wcn->height() != targetHeight) {
                    wcn->resize(targetWidth, targetHeight);
                }
            }
        }
        webNodesToUpdate_.push_back(wcn);
        if (wcn->mode() == WebCanvasNode::Mode::ScreenSpace && wcn->texture()) {
            UIDrawCmd cmd{};
            cmd.position = wcn->screenPosition();
            cmd.size = wcn->screenSize();
            cmd.color = glm::vec4(1.0f);
            cmd.textureId = resources_.ensureBindlessTextureIndex(wcn->texture());
            cmd.hasTexture = 1;
            cmd.sortOrder = wcn->renderOrder();
            drawCmds_.push_back(cmd);
        }
    }
#endif

#ifndef SAIDA_RHI_WEBGPU
    if (!loggedWebCanvasGather_ && !webNodesToUpdate_.empty()) {
        Log::info("UIRenderer: gathered ", webNodesToUpdate_.size(),
                  " WebCanvas node(s), ", drawCmds_.size(), " screen draw command(s)");
        loggedWebCanvasGather_ = true;
    }
#endif

    if (canvas && canvas->isActiveInHierarchy()) {
#ifndef SAIDA_RHI_WEBGPU
        // Desktop draws the non-text UI nodes (colour/image/button/toggle) as
        // bindless quads; the Web player V1 contract registers only Canvas+Text.
        for (auto& child : canvas->children()) {
            if (UINode* uiChild = dynamic_cast<UINode*>(child.get())) {
                traverseUI(uiChild);
            }
        }
#endif
        // Text HUD via the shared CPU RmlUi rasterizer on both platforms.
        gatherHud(*canvas, viewportSize);
    }

    std::stable_sort(drawCmds_.begin(), drawCmds_.end(), [](const UIDrawCmd& a, const UIDrawCmd& b) {
        return a.sortOrder < b.sortOrder;
    });

    SAIDA_PROFILE_COUNTER("UI/WebCanvases", webNodesToUpdate_.size());
    SAIDA_PROFILE_COUNTER("UI/DrawCommands", drawCmds_.size());
}

#ifndef SAIDA_RHI_WEBGPU
void UIRenderer::traverseUI(UINode* node) {
    if (!node->isActiveInHierarchy()) return;

    float globalX = node->globalX();
    float globalY = node->globalY();

    float drawX = globalX - (node->width() * node->pivotX());
    float drawY = globalY - (node->height() * node->pivotY());

    UIDrawCmd cmd{};
    cmd.position = {drawX, drawY};
    cmd.size = {node->width(), node->height()};
    cmd.color = glm::vec4(1.0f);
    cmd.textureId = 0;
    cmd.hasTexture = 0;

    bool draw = false;

    if (auto colorNode = dynamic_cast<UIColorNode*>(node)) {
        cmd.color = colorNode->color();
        draw = true;
    } else if (auto imageNode = dynamic_cast<UIImageNode*>(node)) {
        Texture* tex = resources_.getTexture(imageNode->texture());
        if (tex) {
            cmd.textureId = resources_.ensureBindlessTextureIndex(tex);
            cmd.hasTexture = 1;
            draw = true;
        }
    } else if (auto btnNode = dynamic_cast<UIButtonNode*>(node)) {
        cmd.color = btnNode->currentColor();
        draw = true;
    } else if (auto toggleNode = dynamic_cast<UIToggleNode*>(node)) {
        cmd.color = toggleNode->currentColor();
        draw = true;
    }

    if (draw) {
        drawCmds_.push_back(cmd);
    }

    for (auto& child : node->children()) {
        if (UINode* uiChild = dynamic_cast<UINode*>(child.get())) {
            traverseUI(uiChild);
        }
    }
}
#endif

void UIRenderer::updateAsyncTextures(rhi::CommandEncoder& encoder) {
    SAIDA_PROFILE_FUNCTION();
#ifdef SAIDA_RHI_WEBGPU
    (void)encoder;
#else
    for (auto* wcn : webNodesToUpdate_) {
        wcn->updateTextureIfNeededAsync(encoder);
    }
#endif
}

void UIRenderer::recordCommands(rhi::RenderPassEncoder& rp, uint32_t width, uint32_t height,
                                glm::vec2 viewportOffset, glm::vec2 viewportSize) {
    SAIDA_PROFILE_FUNCTION();
    if (drawCmds_.empty()) return;
    if (viewportSize.x <= 0.0f || viewportSize.y <= 0.0f) {
        viewportSize = {static_cast<float>(width), static_cast<float>(height)};
    }

    rp.setPipeline(*pipeline_);
    rp.setViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));

    const int32_t scissorX = static_cast<int32_t>(std::max(0.0f, std::floor(viewportOffset.x)));
    const int32_t scissorY = static_cast<int32_t>(std::max(0.0f, std::floor(viewportOffset.y)));
    rp.setScissor(scissorX, scissorY,
        std::min(width - static_cast<uint32_t>(scissorX),
                 static_cast<uint32_t>(std::max(1.0f, std::round(viewportSize.x)))),
        std::min(height - static_cast<uint32_t>(scissorY),
                 static_cast<uint32_t>(std::max(1.0f, std::round(viewportSize.y)))));

    // Set 0 matches ui.frag: bindless table on Vulkan, one texture on WebGPU.
#ifndef SAIDA_RHI_WEBGPU
    rp.setBindGroup(0, resources_.globalMaterialSet());
#endif

    UIPushConstants push{};
    push.screenSize = {static_cast<float>(width), static_cast<float>(height)};

    for (const auto& drawCmd : drawCmds_) {
#ifdef SAIDA_RHI_WEBGPU
        if (!drawCmd.textureGroup) continue;
        rp.setBindGroup(0, *drawCmd.textureGroup);
#endif
        push.position = viewportOffset + drawCmd.position;
        push.size = drawCmd.size;
        push.color = drawCmd.color;
        push.textureId = drawCmd.textureId;
        push.hasTexture = drawCmd.hasTexture;
        push.useCorners = drawCmd.useCorners;
        for (int i = 0; i < 4; ++i) push.corners[i] = drawCmd.corners[i];

        rp.setPushConstants(&push, sizeof(UIPushConstants));
        rp.draw(4);
    }
}

} // namespace saida
