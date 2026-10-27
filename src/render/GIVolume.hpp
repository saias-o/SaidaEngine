#pragma once
#include "rhi/Rhi.hpp"
#include "graphics/Buffer.hpp"
#include "graphics/ComputePipeline.hpp"
#include "render/gi/GIClipmap.hpp"
#include "render/gi/GIContributors.hpp"
#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

namespace saida {
class Scene;
class ScenePassFeature;
class MeshNode;
class TerrainRingsNode;
class GpuProfiler;
struct LightingUBO;
struct GIVolumeDesc {
    glm::vec3 spacing{1.2f};
    glm::ivec3 counts{16,8,16};
    int irradianceTexels=6, visibilityTexels=8, voxelResolution=64;
    int raysPerProbe=48, probeBudget=128, brickBudget=16, cascades=1;
    float hysteresis=.8f, distExponent=12.f;
    bool follow=false;
    bool fullResolution=false, trilinear=false, updateAll=false;
    int probeCount() const { return counts.x*counts.y*counts.z*cascades; }
};
// Persistent toroidal atlases. Only scheduled probes and voxel bricks are written.
class GIVolume {
public:
    GIVolume(rhi::Device&,const GIVolumeDesc&,rhi::BindGroupLayout&,rhi::BindGroupLayout&);
    ~GIVolume();
    void prepare(Scene&,glm::vec3 camera,uint32_t frame);
    void fillUniforms(LightingUBO&,const Scene&) const;
    void voxelize(rhi::CommandEncoder&,Scene&,GpuProfiler* =nullptr,const std::vector<std::unique_ptr<ScenePassFeature>>* features=nullptr);
    void update(rhi::CommandEncoder&,const rhi::BindGroup&,GpuProfiler* =nullptr,
                const rhi::BindGroup* scatterGlobal=nullptr);
    rhi::TextureView irradianceView() const { return irradiance_[0]->view(); }
    rhi::TextureView visibilityView() const { return visibility_[0]->view(); }
    rhi::TextureView voxelView() const { return voxelTexture_->view(); }
    rhi::SamplerHandle sampler() const { return sampler_->handle(); }
    const GIVolumeDesc& desc() const { return desc_; }
    bool available() const { return available_; }
    float warmupProgress() const { return grid_.warmupProgress(); }
private:
    void createVoxelResources(rhi::BindGroupLayout&);
    void createComputeResources(rhi::BindGroupLayout&);
    glm::ivec2 atlasSize(int t) const { return {probesPerRow_*(t+2),((desc_.probeCount()+probesPerRow_-1)/probesPerRow_)*(t+2)}; }
    struct Contributor { glm::dvec3 low{},high{}; glm::dmat4 transform{1}; const void* mesh=nullptr; const void* material=nullptr; bool loaded=false; uint64_t seen=0,poseRevision=0; };
    rhi::Device& device_;
    GIVolumeDesc desc_;
    GIClipmap grid_;
    bool available_=false;
    int probesPerRow_=1, frame_=0, probeCount_=0, resetCount_=0;
    uint64_t serial_=0,policyRevision_=0;
    double lastPrepareTime_=0;
    glm::dmat4 worldFromStable_{1};
    std::unordered_map<const MeshNode*,Contributor> contributors_;
    std::vector<MeshNode*> contributorNodes_,collected_;
    std::vector<Contributor*> slots_;  // contributors_ of contributorNodes_, by index
    GIContributorCache contributorCache_;
    bool pendingContributors_ = true;
    std::unordered_map<const TerrainRingsNode*,std::pair<uint64_t,uint64_t>> terrains_;
    std::array<std::unique_ptr<rhi::RenderTexture>,2> irradiance_,visibility_;
    std::unique_ptr<rhi::Sampler> sampler_;
    std::unique_ptr<rhi::RenderTexture> voxelTexture_,normalTexture_,emissionTexture_;
    std::unique_ptr<rhi::BindGroupLayout> voxelSetLayout_,giComputeSetLayout_;
    std::array<std::array<std::unique_ptr<Buffer>,kGIMaxCascades>,2> voxelUbo_;
    std::array<std::array<std::unique_ptr<rhi::BindGroup>,kGIMaxCascades>,2> voxelSets_;
    std::array<std::unique_ptr<Buffer>,2> schedule_,bricks_;
    std::array<std::array<std::unique_ptr<rhi::BindGroup>,2>,2> giComputeSets_;
    std::unique_ptr<Buffer> raysBuffer_;
    std::unique_ptr<rhi::Pipeline> voxelPipeline_;
    std::unique_ptr<ComputePipeline> clearPipeline_,tracePipeline_,blendPipeline_,scatterPipeline_;
#ifdef SAIDA_RHI_WEBGPU
    std::unique_ptr<rhi::RenderTexture> voxelDummyTarget_;
#endif
};
}
