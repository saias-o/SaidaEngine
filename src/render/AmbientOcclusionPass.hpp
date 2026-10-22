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
// pass, from one view's depth, in two stages drawn into two kFormat targets:
// the raw occlusion, each texel from its own turn of the kernel
// (shaders/ssao.glsl), then the visibility (1 unoccluded) averaged over the
// kernel's pattern within each surface (shaders/ssao_denoise.glsl) -- the AO
// map that the scene pass's lit shaders read back on their indirect light
// alone (shaders/screen_ambient_occlusion.glsl).
//
// Like TonemapPass it opens no render pass -- Renderer sequences them -- and
// owns only its pipelines, layouts and sampler. Its inputs are a pair of bind
// groups per view: the desktop's, or one pair per eye layer.
class AmbientOcclusionPass {
public:
    static constexpr rhi::Format kFormat = rhi::Format::R8Unorm;

    enum class Stage {
        Kernel,   // depth -> raw occlusion
        Denoise,  // depth and raw occlusion -> the AO map
    };

    // Every field ambient_occlusion_pass.glsl reads.
    struct PushConstants {
        glm::vec4 settings{0.0f};        // x enabled, y radius (m), z intensity, w power
        glm::vec4 invProjection{0.0f};   // InverseProjection::scale
        glm::vec4 invProjection2{0.0f};  // InverseProjection::depth
        glm::vec4 invOffsets{0.0f};      // InverseProjection::offsets
        glm::vec4 sourceRect{0.0f, 0.0f, 1.0f, 1.0f};  // the viewport in UV of the targets
    };
    static_assert(sizeof(PushConstants) <= 128, "within Vulkan's guaranteed push constant range");

    struct Inputs {
        std::unique_ptr<rhi::BindGroup> kernel;
        std::unique_ptr<rhi::BindGroup> denoise;
    };

    explicit AmbientOcclusionPass(rhi::Device& device);
    ~AmbientOcclusionPass();
    AmbientOcclusionPass(const AmbientOcclusionPass&) = delete;
    AmbientOcclusionPass& operator=(const AmbientOcclusionPass&) = delete;

    // Whether `settings` ask for any occlusion: the depth pre-pass and this
    // pass run only then, and the lit shaders read the map only then.
    static bool active(const SceneSettings& settings);

    // One view's: its single-sample depth and the raw target the kernel writes.
    Inputs makeInputs(rhi::TextureView depth, rhi::TextureView raw) const;

    // Draws `stage`'s fullscreen triangle into an already-open pass whose
    // colour attachment is that stage's kFormat target, confined to
    // `renderRect` (`sourceRect` is that region in normalized coordinates).
    void record(rhi::RenderPassEncoder& rp, Stage stage, const Inputs& inputs,
                const SceneSettings& settings, const glm::mat4& projection,
                const rhi::Rect2D& renderRect, const glm::vec4& sourceRect) const;

    static PushConstants pushConstants(const SceneSettings& settings, const glm::mat4& projection,
                                       const glm::vec4& sourceRect = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));

private:
    rhi::Device& device_;
    std::unique_ptr<rhi::BindGroupLayout> kernelLayout_;
    std::unique_ptr<rhi::BindGroupLayout> denoiseLayout_;
    std::unique_ptr<rhi::Sampler> sampler_;  // nearest: every read is of one texel
    std::unique_ptr<rhi::Pipeline> kernelPipeline_;
    std::unique_ptr<rhi::Pipeline> denoisePipeline_;
};

} // namespace saida
