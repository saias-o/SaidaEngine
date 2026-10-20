#pragma once

#include "rhi/Rhi.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace saida {

class ResourceManager;
class Scene;
class Camera;
class Mesh;
class MeshNode;
class Material;
enum class MaterialType : uint32_t;

struct SceneDraw {
    SceneDraw() = default;
    SceneDraw(Mesh* mesh, Material* material, MeshNode* node, const glm::mat4& world,
              bool castShadows, int32_t boneOffset, MaterialType materialType)
        : mesh(mesh), material(material), node(node), world(world),
          castShadows(castShadows), boneOffset(boneOffset),
          materialType(materialType) {}

    Mesh* mesh = nullptr;
    Material* material = nullptr;
    MeshNode* node = nullptr;
    glm::mat4 world{1.0f};
    bool castShadows = false;
    int32_t boneOffset = -1;
    MaterialType materialType;
    float lodFade = 1.0f;  // MeshNode::lodFade, for the screen-door cross-fade
};

struct EyeRenderInfo {
    rhi::TextureHandle image = {};
    rhi::TextureView imageView = {};
    rhi::Extent2D extent{};
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 eyePosition{0.0f};
};

struct RenderContext {
    rhi::Device& device;
    ResourceManager& resources;
    rhi::BindGroupLayout& globalSetLayout;
    rhi::Format colorFormat;
    rhi::Format depthFormat;
    rhi::SampleCount samples;
    uint32_t viewMask;
    uint32_t framesInFlight;

    bool stereo() const { return viewMask != 0; }
};

struct FrameContext {
    rhi::CommandEncoder& encoder;
    rhi::RenderPassEncoder& pass;
    uint32_t frameIndex;
    const rhi::BindGroup* globalSet;
    Scene& scene;
    float time;
    bool stereo;
    const Camera* camera = nullptr;
    const std::vector<EyeRenderInfo>* eyes = nullptr;
    bool passthrough = false;
    rhi::Extent2D extent{};
    const SceneDraw* draws = nullptr;
    uint32_t drawCount = 0;
};

// Where the terrain hides one directional light, for every lit surface
// (lighting.glsl, `sunOcclusionAt`). The renderer owns the map and binds it
// globally; a feature that knows the terrain (TerrainRingsFeature) writes it
// in its pre-pass and says how to read it. Each texel holds, at its x,z, the
// height below which the light is hidden (r) and how far away what hides it
// stands (g, for the penumbra). Unused, `light` is -1 and nothing is read.
struct SunOcclusionMap {
    static constexpr uint32_t kSize = 256;
    static constexpr rhi::Format kFormat = rhi::Format::RGBA16Float;
    rhi::TextureView view{};      // storage image, kSize x kSize, kFormat
    // World position -> (u, v, height in the map's frame).
    glm::mat4 worldToMap{1.0f};
    int light = -1;               // index in the lighting UBO's lights
};

// The sky as rendered this frame, small and therefore blurred: a 64 x 32
// equirectangular image in VIEW space (u around the camera's up axis, centred
// on its forward direction; v from up to down), with directions below the
// horizon taking the horizon's colour. Owned by the Renderer; the sky feature
// renders it before the scene pass, the tonemap colours the fog with it
// (SceneSettings::fogSkyColor). `written` is reset every frame: a frame with
// no sky leaves the fog its own colour.
struct SkyProbe {
    static constexpr uint32_t kWidth = 64;
    static constexpr uint32_t kHeight = 32;
    static constexpr rhi::Format kFormat = rhi::Format::RGBA16Float;
    rhi::RenderTexture* texture = nullptr;
    rhi::ResourceState state = rhi::ResourceState::Undefined;
    bool written = false;
};

// Compute features run here because neither backend permits compute inside a render pass.
struct PrePassContext {
    rhi::CommandEncoder& encoder;
    uint32_t frameIndex;
    Scene& scene;
    float time;
    bool stereo;
    const Camera* camera = nullptr;
    const std::vector<EyeRenderInfo>* eyes = nullptr;
    rhi::Extent2D extent{};
    SunOcclusionMap* sunOcclusion = nullptr;  // desktop; null where there is none
    SkyProbe* skyProbe = nullptr;             // mono views; null in XR
};

class ScenePassFeature {
public:
    virtual ~ScenePassFeature() = default;
    virtual void createPipelines(const RenderContext& ctx) = 0;
    virtual void recordPrePass(const PrePassContext& pc) { (void)pc; }
    virtual void record(FrameContext& fc) = 0;
};

} // namespace saida
