#pragma once

#include "rhi/Rhi.hpp"

#ifdef SAIDA_RHI_WEBGPU
#include "graphics/Pipeline.hpp"
#endif

#include <glm/glm.hpp>

#include <memory>

namespace saida {

class SceneSettings;

// Screen-space ambient occlusion between the depth pre-pass and the scene
// pass, from one view's depth, in three stages drawn into kFormat targets.
// On the AO grid, one texel per kGridDivisor of the view on each axis: the raw
// occlusion, each texel from its own turn of the kernel (shaders/ssao.glsl),
// then its average over the kernel's pattern within each surface
// (shaders/ssao_denoise.glsl). At full resolution, that grid brought back to
// every texel of the view by depth (shaders/ssao_upsample.glsl): the
// visibility (1 unoccluded) the scene pass's lit shaders read back on their
// indirect light alone (shaders/screen_ambient_occlusion.glsl). Every depth
// read is the full resolution's.
//
// Like TonemapPass it opens no render pass -- Renderer sequences them -- and
// owns only its pipelines, layouts and sampler. Its inputs are one bind group
// per stage and view: the desktop's, or one set per eye layer.
class AmbientOcclusionPass {
public:
    static constexpr rhi::Format kFormat = rhi::Format::R8Unorm;
    static constexpr uint32_t kGridDivisor = 2;  // AO_RESOLUTION_DIVISOR in ssao.glsl

    enum class Stage {
        Kernel,    // depth -> raw occlusion, on the grid
        Denoise,   // depth and raw occlusion -> denoised occlusion, on the grid
        Upsample,  // depth and denoised occlusion -> the AO map, full resolution
    };

    // Every field ambient_occlusion_pass.glsl reads.
    struct PushConstants {
        glm::vec4 settings{0.0f};        // x enabled, y radius (m), z intensity, w power
        glm::vec4 invProjection{0.0f};   // InverseProjection::scale
        glm::vec4 invProjection2{0.0f};  // InverseProjection::depth
        glm::vec4 invOffsets{0.0f};      // InverseProjection::offsets
        glm::vec4 sourceRect{0.0f, 0.0f, 1.0f, 1.0f};  // the viewport in UV of the depth target
        glm::vec4 aoGrid{0.0f};  // xy the viewport's origin in grid texels of the targets, zw its size
    };
    static_assert(sizeof(PushConstants) <= 128, "within Vulkan's guaranteed push constant range");

    struct Inputs {
        std::unique_ptr<rhi::BindGroup> kernel;
        std::unique_ptr<rhi::BindGroup> denoise;
        std::unique_ptr<rhi::BindGroup> upsample;
    };

    explicit AmbientOcclusionPass(rhi::Device& device);
    ~AmbientOcclusionPass();
    AmbientOcclusionPass(const AmbientOcclusionPass&) = delete;
    AmbientOcclusionPass& operator=(const AmbientOcclusionPass&) = delete;

    // Whether `settings` ask for any occlusion: the depth pre-pass and this
    // pass run only then, and the lit shaders read the map only then.
    static bool active(const SceneSettings& settings);

    // One view's: its single-sample depth, and the grid targets the kernel and
    // denoise stages write.
    Inputs makeInputs(rhi::TextureView depth, rhi::TextureView raw, rhi::TextureView denoised) const;

    // A grid target's size, for a full-resolution target of `full`.
    static rhi::Extent2D gridExtent(rhi::Extent2D full);
    // Where a grid stage draws, for a view drawn in `viewport` of the full targets.
    static rhi::Rect2D gridRect(const rhi::Rect2D& viewport);
    // Where `stage` draws, for a view drawn in `viewport`: the grid or the viewport.
    static rhi::Rect2D stageRect(Stage stage, const rhi::Rect2D& viewport);

    // Draws `stage`'s fullscreen triangle into an already-open pass whose
    // colour attachment is that stage's kFormat target, confined to
    // stageRect(stage, viewport) (`sourceRect` is the viewport in normalized
    // coordinates of the depth).
    void record(rhi::RenderPassEncoder& rp, Stage stage, const Inputs& inputs,
                const SceneSettings& settings, const glm::mat4& projection,
                const rhi::Rect2D& viewport, const glm::vec4& sourceRect) const;

    static PushConstants pushConstants(const SceneSettings& settings, const glm::mat4& projection,
                                       const rhi::Rect2D& viewport,
                                       const glm::vec4& sourceRect = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));

private:
    rhi::Device& device_;
    std::unique_ptr<rhi::BindGroupLayout> kernelLayout_;
    std::unique_ptr<rhi::BindGroupLayout> pairLayout_;  // depth and a grid target
    std::unique_ptr<rhi::Sampler> sampler_;  // nearest: every read is of one texel
    std::unique_ptr<rhi::Pipeline> kernelPipeline_;
    std::unique_ptr<rhi::Pipeline> denoisePipeline_;
    std::unique_ptr<rhi::Pipeline> upsamplePipeline_;
};

} // namespace saida
