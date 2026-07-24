#pragma once

#include "render/RenderFeature.hpp"
#include "graphics/Pipeline.hpp"
#include "graphics/Texture.hpp"
#include "project/AssetRegistry.hpp"
#include "rhi/Rhi.hpp"

#include <glm/glm.hpp>

#include <memory>

namespace saida {

// Equirectangular skybox. Owns its own descriptor set (the environment texture)
// and draws a fullscreen triangle at the far plane. Handles both the mono (desktop)
// and stereo (XR multiview, per-eye via gl_ViewIndex) paths; in XR passthrough the
// sky is skipped so the real world shows through.
class SkyboxFeature : public ScenePassFeature {
public:
    ~SkyboxFeature() override;
    void createPipelines(const RenderContext& ctx) override;
    void record(FrameContext& fc) override;

private:
    struct MonoPush { glm::mat4 invViewProj; float exposure; float rotation; };
    struct StereoPush { glm::mat4 invViewProj[2]; float exposure; float rotation; };

    rhi::Device* device_ = nullptr;
    ResourceManager* resources_ = nullptr;
    bool stereo_ = false;

    std::unique_ptr<Pipeline> pipeline_;
    std::unique_ptr<rhi::BindGroupLayout> setLayout_;
    std::unique_ptr<rhi::BindGroup> set_;
    AssetID currentTexture_ = kAssetInvalid;
    // The same AssetID can be reborn on a different Texture object after an
    // eviction (trimUnused) + a reload: also compare the pointer.
    const Texture* currentTexturePtr_ = nullptr;
};

} // namespace saida
