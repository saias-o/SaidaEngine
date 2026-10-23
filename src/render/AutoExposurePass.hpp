#pragma once

#include "graphics/Buffer.hpp"
#include "graphics/ComputePipeline.hpp"
#include "rhi/Rhi.hpp"

#include <glm/glm.hpp>

#include <memory>

namespace saida {

class SceneSettings;

// Meters HDR with a centre-weighted luminance histogram and adapts exposure
// within the scene's EV limits. Renderer records it before tonemapping, which
// reads the 16-byte GPU state without CPU readback.
class AutoExposurePass {
public:
    struct PushConstants {
        glm::vec4 sourceRect{0.0f, 0.0f, 1.0f, 1.0f};  // the viewport in UV of the HDR target
        glm::vec4 range{0.0f};  // x lowest EV, y highest EV, z log2 key, w this frame's share
    };

    explicit AutoExposurePass(rhi::Device& device);
    ~AutoExposurePass();
    AutoExposurePass(const AutoExposurePass&) = delete;
    AutoExposurePass& operator=(const AutoExposurePass&) = delete;

    // The view metered: the HDR target, once per (re)creation.
    void setInput(rhi::TextureView hdr);
    // What the tonemap binds (x: the EV applied).
    const Buffer& state() const { return *state_; }

    // Meters the HDR (already in ShaderRead) for the view `sourceRect` and
    // adapts over `deltaSeconds`; the caller orders it before the tonemap.
    void record(rhi::CommandEncoder& encoder, const SceneSettings& settings,
                const glm::vec4& sourceRect, float deltaSeconds) const;

    static PushConstants pushConstants(const SceneSettings& settings, const glm::vec4& sourceRect,
                                       float deltaSeconds);

private:
    std::unique_ptr<rhi::BindGroupLayout> layout_;
    std::unique_ptr<rhi::Sampler> sampler_;
    std::unique_ptr<Buffer> state_;
    std::unique_ptr<ComputePipeline> pipeline_;
    std::unique_ptr<rhi::BindGroup> set_;
};

} // namespace saida
