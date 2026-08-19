#pragma once

#include "rhi/Rhi.hpp"

#ifdef SAIDA_RHI_WEBGPU
#include "graphics/Pipeline.hpp"
#endif

#include <glm/glm.hpp>

#include <memory>

namespace saida {

class SceneSettings;

// Resolves the HDR scene target onto the swapchain: the tonemap curve itself,
// plus the AO and fog it evaluates from depth and the bloom it composites.
//
// It does NOT open a render pass. The same pass carries the UI and the editor
// overlay afterwards, and only Renderer::drawFrame sequences that (ROADMAP §3:
// passes never call each other). So the boundary is exactly the one §3 asks
// for — input is a set of views, output is whatever pass the caller began —
// and this unit owns nothing but its own pipeline, layout, samplers and set.
class TonemapPass {
public:
    // Every field the shader reads. Public because the XR path builds one too,
    // through the static builder below.
    struct PushConstants {
        glm::mat4 invProjection{1.0f};
        glm::vec4 aoParams{0.0f};        // x enabled, y radius, z intensity, w power
        glm::vec4 fogColor{0.0f};
        glm::vec4 fogParams{0.0f};       // x enabled, y start, z density, w exposure
        glm::vec4 bloomParams{0.0f};     // x enabled, y threshold, z intensity, w radius px
        glm::vec4 sourceRect{0.0f, 0.0f, 1.0f, 1.0f};
        glm::vec4 projectionParams{0.0f};
        glm::vec4 projectionParams2{0.0f};
    };

    TonemapPass(rhi::Device& device, rhi::Format outputFormat);
    ~TonemapPass();
    TonemapPass(const TonemapPass&) = delete;
    TonemapPass& operator=(const TonemapPass&) = delete;

    // Point the pass at the current render targets. Called again whenever they
    // are recreated; until it has been called once there is no set to bind and
    // ready() is false.
    void setInputs(rhi::TextureView hdr, rhi::TextureView depth, rhi::TextureView bloom,
                   rhi::SamplerHandle bloomSampler);
    bool ready() const { return pipeline_ && set_; }

    // Draws the fullscreen triangle into an already-open pass, confined to
    // `renderRect` (the editor renders into a sub-rectangle of the target;
    // `sourceRect` is that same region in normalized coordinates).
    //
    // Viewport and scissor are set here, after the pipeline is bound, in the
    // order the pre-extraction code recorded them. Whether binding a pipeline
    // resets that state is not something the RHI documents, so the order is
    // preserved deliberately rather than assumed irrelevant.
    void record(rhi::RenderPassEncoder& rp, const SceneSettings& settings,
                const glm::mat4& projection, const rhi::Rect2D& renderRect,
                const glm::vec4& sourceRect, float exposure) const;

    // Static: the XR tonemap draws this same shader once per eye and still
    // lives in Renderer until XrRenderer is extracted (ROADMAP §3). It needs
    // the same constants without owning an instance, and sharing the builder is
    // what keeps one shader from drifting into two interpretations.
    static PushConstants pushConstants(const SceneSettings& settings,
                                       const glm::mat4& projection, float exposure);

private:
    rhi::Device& device_;
    std::unique_ptr<rhi::BindGroupLayout> setLayout_;
    std::unique_ptr<rhi::Sampler> colorSampler_;   // linear (HDR + bloom)
    std::unique_ptr<rhi::Sampler> depthSampler_;   // nearest (AO depth)
    std::unique_ptr<rhi::Pipeline> pipeline_;
    std::unique_ptr<rhi::BindGroup> set_;
};

} // namespace saida
