#pragma once

#include "render/RenderFeature.hpp"
#include "graphics/Buffer.hpp"    // Buffer is a class (Vulkan) or alias (web) — not fwd-declarable
#include "graphics/Pipeline.hpp"
#include "rhi/Rhi.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace saida {

// Render feature for WaterNode: procedural grid, set 0 globals, set 1 water data.
class WaterFeature : public ScenePassFeature {
public:
    ~WaterFeature() override;
    void createPipelines(const RenderContext& ctx) override;
    void record(FrameContext& fc) override;

private:
    // Mirrors GpuWater in water_common.glsl; all-vec4 keeps std140 padding explicit.
    struct GpuWater {
        glm::vec4 area;        // centreX, surfaceY, centreZ, halfSize
        glm::vec4 deep;        // rgb deep colour, w roughness
        glm::vec4 foam;        // rgb foam colour, w reflectivity
        glm::vec4 waveA;       // amp, wavelength, speed, choppiness
        glm::vec4 detail1;     // scale, speed, strength, angleDeg
        glm::vec4 detail2;     // scale, speed, strength, angleDeg
        glm::vec4 look;        // fresnelPower, specularPower, specularIntensity, foamThreshold
        glm::vec4 misc;        // warpAmount, detailFadeDistance, foamIntensity, depthColorFalloff
        glm::vec4 shoreColor;  // rgb shallow colour, w edgeFadeDepth
        glm::vec4 shoreGeom;   // beach(dirX,dirZ,waterlineDist,slope) | lake(cx,cz,radius,slope)
        glm::vec4 shoreTune;   // foamWidth, swashSpeed, swashAmount, waveFlattenDepth
        glm::vec4 shoreMode;   // mode, shoreFoamIntensity, style, reserved
        glm::vec4 cartoonWave;
        glm::vec4 cartoonDetail;
        glm::vec4 cartoonLook;
        glm::vec4 cartoonShore;
    };

    // Tiny per-draw push: which water entry + the animation clock.
    struct Push {
        uint32_t index;
        float time;
    };

    static constexpr uint32_t kGridRes = 128;  // must match RES in water.vert
    static constexpr uint32_t kCartoonVertexCount = 6;
    static constexpr uint32_t kMaxWaters = 8;  // must match WATER_MAX in water_common.glsl

    rhi::Device* device_ = nullptr;
    std::unique_ptr<Pipeline> realisticPipeline_;
    std::unique_ptr<Pipeline> cartoonPipeline_;

    // set 1: a UBO array of GpuWater, double-buffered per frame-in-flight.
    std::unique_ptr<rhi::BindGroupLayout> setLayout_;
    std::vector<std::unique_ptr<Buffer>> ubos_;     // one per frame-in-flight
    std::vector<std::unique_ptr<rhi::BindGroup>> sets_;  // one per frame-in-flight
};

} // namespace saida
