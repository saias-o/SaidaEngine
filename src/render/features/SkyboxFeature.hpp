#pragma once

#include "render/RenderFeature.hpp"
#include "graphics/Pipeline.hpp"
#include "graphics/Texture.hpp"
#include "project/AssetRegistry.hpp"
#include "rhi/Rhi.hpp"

#include <glm/glm.hpp>

#include <memory>

namespace saida {

class SceneSettings;

// Equirectangular skybox. Owns its own descriptor set (the environment texture,
// and the second sky it crossfades into — `SceneSettings::skyboxBlend`) and draws a fullscreen triangle at the far plane. Handles both the mono (desktop)
// and stereo (XR multiview, per-eye via gl_ViewIndex) paths; in XR passthrough the
// sky is skipped so the real world shows through. A runtime storm-cloud cover
// overlays the HDR sky. Fog blends the lower sky in linear HDR before the Sun
// disc; the upper photograph and authored textures remain intact.
class SkyboxFeature : public ScenePassFeature {
public:
    ~SkyboxFeature() override;
    void createPipelines(const RenderContext& ctx) override;
    // Renders the sky probe (RenderFeature.hpp) when the fog asks for the
    // sky's colour: mono views only.
    void recordPrePass(const PrePassContext& pc) override;
    void record(FrameContext& fc) override;

private:
    struct MonoPush {
        glm::mat4 invViewProj; float exposure; float rotation; float blend; float blendRotation;
        glm::vec4 sunDirection; glm::vec4 sunColor;
        glm::vec4 fogColor; glm::vec4 fogUp;
    };
    struct StereoPush {
        glm::mat4 invViewProj[2]; float exposure; float rotation; float blend; float blendRotation;
        glm::vec4 sunDirection; glm::vec4 sunColor;
        glm::vec4 fogColor; glm::vec4 fogUp;
    };
    static_assert(sizeof(MonoPush) == 144, "Skybox mono push layout must match GLSL");
    static_assert(sizeof(StereoPush) == 208, "Skybox XR push layout must match GLSL");

    rhi::Device* device_ = nullptr;
    ResourceManager* resources_ = nullptr;
    bool stereo_ = false;

    // Binds the current sky (and the one it fades into); false without one.
    bool bindSky(const SceneSettings& settings, float& blend);
    MonoPush monoPush(const SceneSettings& settings, const glm::mat4& invViewProj, float blend) const;

    std::unique_ptr<Pipeline> pipeline_;
    std::unique_ptr<Pipeline> probePipeline_;
    std::unique_ptr<rhi::BindGroupLayout> setLayout_;
    std::unique_ptr<rhi::BindGroup> set_;
    AssetID currentTexture_ = kAssetInvalid;
    AssetID currentBlend_ = kAssetInvalid;
    // The same AssetID can be reborn on a different Texture object after an
    // eviction (trimUnused) + a reload: also compare the pointers.
    const Texture* currentTexturePtr_ = nullptr;
    const Texture* currentBlendPtr_ = nullptr;
};

} // namespace saida
