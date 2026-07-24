#pragma once

#include "render/RenderFeature.hpp"
#include "graphics/Pipeline.hpp"

#include <glm/glm.hpp>

#include <memory>

namespace saida {

// Ultra-cheap per-mesh outline: redraws only opted-in meshes as an inverted hull,
// offset in screen space for pixel-stable thickness across desktop, mobile and XR.
// It preserves the real material because it is an additive scene pass, not a
// material variant.
class OutlineFeature : public ScenePassFeature {
public:
    void createPipelines(const RenderContext& ctx) override;
    void record(FrameContext& fc) override;

private:
    struct Push {
        glm::mat4 model{1.0f};
        glm::vec4 color{0.0f, 0.0f, 0.0f, 1.0f};
        glm::vec4 params{0.0f};  // x widthPx, y boneOffset, zw viewport size
        glm::vec4 localCenter{0.0f};
        glm::vec4 localHalfExtent{1.0f};
    };

    std::unique_ptr<Pipeline> pipeline_;
};

} // namespace saida
