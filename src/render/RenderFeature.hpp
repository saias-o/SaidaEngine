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
};

class ScenePassFeature {
public:
    virtual ~ScenePassFeature() = default;
    virtual void createPipelines(const RenderContext& ctx) = 0;
    virtual void recordPrePass(const PrePassContext& pc) { (void)pc; }
    virtual void record(FrameContext& fc) = 0;
};

} // namespace saida
