#pragma once

#include "render/RenderFeature.hpp"
#include "graphics/Pipeline.hpp"
#include "graphics/Buffer.hpp"

#include <memory>
#include <vector>

namespace saida {

// Editor debug aid (desktop/mono only): draws skeleton bones as lines from every
// Animator when SceneSettings::showSkeletons is on. Owns a per-frame dynamic vertex
// buffer. A no-op under stereo/XR.
class DebugLinesFeature : public ScenePassFeature {
public:
    void createPipelines(const RenderContext& ctx) override;
    void record(FrameContext& fc) override;

private:
    static constexpr uint32_t kMaxVerts = 16384;
    std::unique_ptr<Pipeline> pipeline_;
    std::vector<std::unique_ptr<Buffer>> buffers_;  // one per frame in flight
};

} // namespace saida
