#include "render/GIVolume.hpp"
#ifndef SAIDA_RHI_WEBGPU
#include "graphics/VulkanDevice.hpp"
#endif
#include "render/Renderer.hpp"
#include "graphics/Mesh.hpp"
#include "graphics/Material.hpp"
#include "graphics/GpuProfiler.hpp"
#include "scene/Scene.hpp"
#include "nodes/MeshNode.hpp"
#include "nodes/TerrainRingsNode.hpp"
#include "physics/CollisionObjectNode.hpp"
#include "core/Paths.hpp"
#include "core/Log.hpp"
#include "core/Time.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace saida {
namespace {
constexpr auto kAtlasFormat=rhi::Format::RGBA16Float;
constexpr auto kUsage=rhi::TextureUsage::Storage|rhi::TextureUsage::Sampled|rhi::TextureUsage::CopyDst;
struct VoxelData { glm::vec4 origin,extent; glm::ivec4 res,wrap; glm::mat4 worldToGrid,axisVP[3]; };
struct VoxelPush { glm::mat4 model; glm::vec4 params; };
struct TracePush { int raysPerProbe,probeCount; };
struct BlendPush { int mode,raysPerProbe,probeCount; float hysteresis,distExponent; };
struct ScatterPush { int mode,probeCount,offset; };
GIConfig configOf(const GIVolumeDesc& d) {
    GIConfig c; c.probes=d.counts; c.voxels={d.voxelResolution,d.voxelResolution/2,d.voxelResolution};
    c.cascades=d.cascades;c.spacing=d.spacing.x;
    c.probeBudget=d.probeBudget;c.brickBudget=d.brickBudget;c.follow=d.follow;
    c.updateAll=d.updateAll;
    if(c.updateAll) c.catchUp=1;
    return c;
}
rhi::BindGroupEntry imageEntry(int b,rhi::TextureView v,bool storage=false) {
    rhi::BindGroupEntry e{};e.binding=b;e.view=v;
#ifndef SAIDA_RHI_WEBGPU
    e.textureState=storage?rhi::ResourceState::StorageReadWrite:rhi::ResourceState::ShaderRead;
#endif
    return e;
}
rhi::BindGroupEntry bufferEntry(int b,Buffer& buffer) { rhi::BindGroupEntry e{};e.binding=b;e.buffer=&buffer;return e; }
}
GIVolume::GIVolume(rhi::Device& device,const GIVolumeDesc& desc,
                  rhi::BindGroupLayout& materialLayout,rhi::BindGroupLayout& globalLayout)
 :device_(device),desc_(desc),grid_(configOf(desc)),available_(device.capabilities().fragmentStorageWrites) {
    probesPerRow_=int(std::ceil(std::sqrt(double(desc_.probeCount()))));
    sampler_=std::make_unique<rhi::Sampler>(device_,rhi::SamplerDesc{});
    auto texture=[&](glm::ivec3 size,rhi::Format format) {
        rhi::RenderTextureDesc d;d.width=size.x;d.height=size.y;d.depth=size.z;d.format=format;d.usage=kUsage;
        return std::make_unique<rhi::RenderTexture>(device_,d);
    };
    for(int i=0;i<2;++i) {
        irradiance_[i]=texture(glm::ivec3(atlasSize(desc_.irradianceTexels),1),kAtlasFormat);
        visibility_[i]=texture(glm::ivec3(atlasSize(desc_.visibilityTexels),1),kAtlasFormat);
    }
    auto size=grid_.config().voxels;size.z*=desc_.cascades;
    voxelTexture_=texture(size,rhi::Format::RGBA8Unorm);
    normalTexture_=texture(size,rhi::Format::RGBA8Unorm);
    emissionTexture_=texture(size,kAtlasFormat);
    device_.withSingleTimeEncoder([&](rhi::CommandEncoder& e) {
        auto clear=[&](rhi::RenderTexture& t,bool storage) {
            e.transition(t.image(),rhi::ResourceState::Undefined,rhi::ResourceState::CopyDst);
            e.clearColorTexture(t.image(),{0,0,0,0});
            e.transition(t.image(),rhi::ResourceState::CopyDst,storage?rhi::ResourceState::StorageReadWrite:rhi::ResourceState::ShaderRead);
        };
        for(int i=0;i<2;++i) {clear(*irradiance_[i],true);clear(*visibility_[i],true);}
        clear(*voxelTexture_,false);clear(*normalTexture_,false);clear(*emissionTexture_,false);
    });
    if(!available_) {Log::warn("GI unavailable: fragment storage writes are unsupported");return;}
    for(int f=0;f<2;++f) {
        schedule_[f]=std::make_unique<Buffer>(device_,uint64_t(desc_.probeCount()+grid_.config().maxProbeBudget())*16,rhi::BufferUsage::Storage,MemoryUsage::HostVisible);
        bricks_[f]=std::make_unique<Buffer>(device_,uint64_t(grid_.config().bricksPerCascade()*desc_.cascades+desc_.brickBudget)*4,rhi::BufferUsage::Storage,MemoryUsage::HostVisible);
    }
    createVoxelResources(materialLayout);createComputeResources(globalLayout);
    Log::info("GI: ",desc_.cascades," cascades, ",desc_.probeCount()," probes; frame budgets ",desc_.probeBudget," probes / ",desc_.brickBudget," voxel bricks");
    Log::info("GI quality: ",desc_.raysPerProbe," rays/probe, ",desc_.voxelResolution," voxel width, ",
        desc_.fullResolution?"per-pixel":"half-resolution"," lighting, ",desc_.trilinear?8:4," probe taps, ",
        desc_.updateAll?"all probes and queued bricks each frame":"amortized work budgets");
}
GIVolume::~GIVolume()=default;

void GIVolume::prepare(Scene& scene,glm::vec3 camera,uint32_t frame) {
    if(!available_) return;
    SAIDA_PROFILE_SCOPE("GI/Prepare");
    frame_=int(frame%2);++serial_;int invalidations=0;
    const bool rebased=worldFromStable_!=scene.worldFromStable();worldFromStable_=scene.worldFromStable();
    const bool policyChanged=policyRevision_!=scene.subtreeRevision();policyRevision_=scene.subtreeRevision();
    const auto stableFromWorld=glm::inverse(worldFromStable_);
    const double now=Time::unscaledElapsed();
    grid_.move(glm::dvec3(stableFromWorld*glm::dvec4(camera,1)),now-lastPrepareTime_);
    lastPrepareTime_=now;
    // Cache stable bounds, so rebasing the world does not dirty stationary geometry.
    for(MeshNode* node:scene.meshes()) {
        Mesh* mesh=node->sourceMesh();Material* material=node->material();
        if(!mesh || !material || mesh->skinned() || material->desc().billboardColumns || !node->meshEnabled()) continue;
        auto found=contributors_.find(node);
        if(!policyChanged && found!=contributors_.end()) {
            auto& cached=found->second;cached.seen=serial_;
            if(!rebased && cached.poseRevision==node->subtreeTransformRevision() && cached.mesh==mesh &&
               cached.material==material && cached.loaded==mesh->loaded()) continue;
        }
        bool visible=true,dynamic=false;
        for(Node* ancestor=node;ancestor;ancestor=ancestor->parent()) {
            visible &= ancestor->visible() && ancestor->enabled();
            if(auto* body=ancestor->asCollisionObject()) dynamic |= body->motion()!=BodyMotion::Static;
        }
        if(!visible || dynamic) {
            if(found!=contributors_.end()) {++invalidations,grid_.invalidate(found->second.low,found->second.high);contributors_.erase(found);}
            continue;
        }
        auto& c=contributors_[node];c.seen=serial_;
        if(!rebased && c.poseRevision==node->subtreeTransformRevision() && c.mesh==mesh &&
           c.material==material && c.loaded==mesh->loaded()) continue;
        c.poseRevision=node->subtreeTransformRevision();
        const auto transform=stableFromWorld*glm::dmat4(node->worldTransform());
        double error=0;for(int a=0;a<4;++a)for(int b=0;b<4;++b) error=std::max(error,std::abs(transform[a][b]-c.transform[a][b]));
        if(c.mesh==mesh && c.material==material && c.loaded==mesh->loaded() && error<.002) continue;
        if(c.mesh) ++invalidations,grid_.invalidate(c.low,c.high);
        c.mesh=mesh;c.material=material;c.loaded=mesh->loaded();c.transform=transform;
        const auto bounds=mesh->bounds();const glm::dvec3 center=glm::dvec3(transform*glm::dvec4(bounds.center(),1));
        const glm::dmat3 m(transform);const auto extent=glm::abs(m[0])*double(bounds.extent().x)*.5+glm::abs(m[1])*double(bounds.extent().y)*.5+glm::abs(m[2])*double(bounds.extent().z)*.5;
        c.low=center-extent;c.high=center+extent;++invalidations,grid_.invalidate(c.low,c.high);
    }
    for(auto it=contributors_.begin();it!=contributors_.end();) {
        if(it->second.seen!=serial_) {++invalidations,grid_.invalidate(it->second.low,it->second.high);it=contributors_.erase(it);} else ++it;
    }
    bool terrainChanged=false;
    for(const auto* terrain:scene.terrainRings()) {
        if(!terrain->isActiveInHierarchy() || !terrain->isVisibleInHierarchy()) continue;
        uint64_t signature=terrain->layersRevision();
        for(const auto& hole:terrain->holes()) for(const auto& point:hole)
            for(double value:{point.x,point.y}) signature^=std::hash<double>{}(value)+0x9e3779b97f4a7c15ull+(signature<<6)+(signature>>2);
        for(int k=0;k<TerrainRingsNode::kMaxLevels;++k)
            signature^=terrain->revision(k)+0x9e3779b97f4a7c15ull+(signature<<6)+(signature>>2);
        auto& state=terrains_[terrain];terrainChanged|=state.first!=signature;
        state={signature,serial_};
    }
    for(auto it=terrains_.begin();it!=terrains_.end();) {
        if(it->second.second!=serial_) {terrainChanged=true;it=terrains_.erase(it);} else ++it;
    }
    SAIDA_PROFILE_COUNTER("GI/TerrainChanged",terrainChanged?1:0);
    if(terrainChanged) for(int k=0;k<desc_.cascades;++k) {
        const auto& c=grid_.cascade(k);++invalidations,grid_.invalidate(c.origin,c.origin+glm::dvec3(desc_.counts)*c.spacing);
    }
    grid_.plan();probeCount_=int(grid_.probes().size());resetCount_=int(grid_.invalidProbes().size());
    const int maxBudget=grid_.config().maxProbeBudget();
    std::vector<glm::ivec4> schedule(maxBudget+resetCount_);
    std::copy(grid_.probes().begin(),grid_.probes().end(),schedule.begin());
    for(int i=0;i<resetCount_;++i) schedule[maxBudget+i]=glm::ivec4(grid_.invalidProbes()[i],0,0,0);
    if(!schedule.empty()) schedule_[frame_]->write(schedule.data(),schedule.size()*sizeof(schedule[0]));
    auto brickData=grid_.brickMask();brickData.insert(brickData.end(),grid_.bricks().begin(),grid_.bricks().end());
    bricks_[frame_]->write(brickData.data(),brickData.size()*4);
    scene.settings().giWarmupProgress=grid_.warmupProgress();
    SAIDA_PROFILE_COUNTER("GI/ProbeUpdates",probeCount_);
    SAIDA_PROFILE_COUNTER("GI/VoxelBricks",grid_.bricks().size());
    SAIDA_PROFILE_COUNTER("GI/QueuedBricks",grid_.queuedBricks());
    SAIDA_PROFILE_COUNTER("GI/Invalidations",invalidations);
    SAIDA_PROFILE_COUNTER("GI/Warmup",grid_.warmupProgress());
    const glm::mat4 rotation=glm::mat4(glm::dmat4(glm::dmat3(stableFromWorld)));
    for(int k=0;k<desc_.cascades;++k) {
        const auto& c=grid_.cascade(k);VoxelData u{};
        u.origin=glm::vec4(glm::vec3(worldFromStable_*glm::dvec4(c.origin,1)),0);
        u.extent=glm::vec4(glm::vec3(desc_.counts)*float(c.spacing),0);
        u.res=glm::ivec4(grid_.config().voxels,k);u.wrap=glm::ivec4(c.voxelWrap,0);u.worldToGrid=rotation;
        const auto center=u.extent*.5f;
        auto axis=[&](glm::vec3 direction,glm::vec3 up,float w,float h,float depth) {
            return glm::ortho(-w*.5f,w*.5f,-h*.5f,h*.5f,0.f,depth*2.f)*glm::lookAt(glm::vec3(center)+direction*depth,glm::vec3(center),up)*rotation*glm::translate(glm::mat4(1),-glm::vec3(u.origin));
        };
        u.axisVP[0]=axis({0,0,1},{0,1,0},u.extent.x,u.extent.y,u.extent.z);
        u.axisVP[1]=axis({1,0,0},{0,1,0},u.extent.z,u.extent.y,u.extent.x);
        u.axisVP[2]=axis({0,1,0},{0,0,1},u.extent.x,u.extent.z,u.extent.y);
        voxelUbo_[frame_][k]->write(&u,sizeof(u));
    }
}
void GIVolume::fillUniforms(LightingUBO& u,const Scene& scene) const {
    u.giOrigin=glm::vec4(0,0,0,scene.settings().giEnabled && available_?1.f:0.f);
    u.giSpacing=glm::vec4(desc_.spacing,scene.settings().giIntensity);
    u.giCounts=glm::ivec4(desc_.counts,probesPerRow_);
    // SAIDA_GI_DEBUG=voxels|irradiance: the editor's voxel view, or what surfaces receive.
    static const int debugView=[]{const char* v=std::getenv("SAIDA_GI_DEBUG");return !v?0:std::string(v)=="voxels"?1:std::string(v)=="irradiance"?2:0;}();
    u.giAtlas={desc_.irradianceTexels,desc_.visibilityTexels,scene.settings().giDebugVoxels?1:debugView,desc_.voxelResolution};
    u.giClipmap={desc_.cascades,grid_.config().voxels.y,desc_.probeBudget,desc_.raysPerProbe>=64?256:desc_.raysPerProbe>=48?128:desc_.raysPerProbe>=40?64:32};
    u.giWorldToGrid=glm::mat4(glm::dmat4(glm::transpose(glm::dmat3(scene.worldFromStable()))));
    for(int k=0;k<desc_.cascades;++k) {
        const auto& c=grid_.cascade(k);
        u.giCascadeOrigins[k]=glm::vec4(glm::vec3(scene.worldFromStable()*glm::dvec4(c.origin,1)),float(c.spacing));
        u.giCascadeWrap[k]=glm::ivec4(c.probeWrap,desc_.trilinear?1:0);
    }
    u.giOrigin=glm::vec4(glm::vec3(u.giCascadeOrigins[0]),u.giOrigin.w);
}
void GIVolume::createVoxelResources(rhi::BindGroupLayout& materialLayout) {
    const auto stages=rhi::ShaderStages::Fragment|rhi::ShaderStages::Compute;
#ifdef SAIDA_RHI_WEBGPU
    std::vector<rhi::webgpu::BindGroupLayoutEntry> entries;
    for(int b:{0,2,3}) {rhi::webgpu::BindGroupLayoutEntry e{};e.binding=b;e.type=rhi::BindingType::StorageImage;e.visibility=stages;e.dim=rhi::webgpu::TextureDim::Dim3D;e.storageFormat=b==3?kAtlasFormat:rhi::Format::RGBA8Unorm;e.storageAccess=WGPUStorageTextureAccess_WriteOnly;entries.push_back(e);}
    rhi::webgpu::BindGroupLayoutEntry u{};u.binding=1;u.type=rhi::BindingType::UniformBuffer;u.visibility=stages|rhi::ShaderStages::Vertex;entries.push_back(u);
    rhi::webgpu::BindGroupLayoutEntry b{};b.binding=4;b.type=rhi::BindingType::StorageBuffer;b.visibility=stages;entries.push_back(b);
#else
    std::vector<rhi::BindGroupLayoutEntry> entries={{0,rhi::BindingType::StorageImage,stages},{1,rhi::BindingType::UniformBuffer,stages|rhi::ShaderStages::Vertex},{2,rhi::BindingType::StorageImage,stages},{3,rhi::BindingType::StorageImage,stages},{4,rhi::BindingType::StorageBuffer,stages}};
#endif
    voxelSetLayout_=std::make_unique<rhi::BindGroupLayout>(device_,entries);
    for(int f=0;f<2;++f)for(int k=0;k<desc_.cascades;++k) {
        voxelUbo_[f][k]=std::make_unique<Buffer>(device_,sizeof(VoxelData),rhi::BufferUsage::Uniform,MemoryUsage::HostVisible);
        voxelSets_[f][k]=std::make_unique<rhi::BindGroup>(*voxelSetLayout_,std::vector<rhi::BindGroupEntry>{imageEntry(0,voxelTexture_->view(),true),bufferEntry(1,*voxelUbo_[f][k]),imageEntry(2,normalTexture_->view(),true),imageEntry(3,emissionTexture_->view(),true),bufferEntry(4,*bricks_[f])});
    }
    rhi::Pipeline::Desc d;
#ifdef SAIDA_RHI_WEBGPU
    d.vertPath="/shaders/voxelize.vert.wgsl";d.fragPath="/shaders/voxelize.frag.wgsl";
    d.colorFormats={rhi::Format::RGBA8Unorm};d.colorWrite=false;
    rhi::RenderTextureDesc target;target.width=target.height=desc_.voxelResolution;target.format=rhi::Format::RGBA8Unorm;target.usage=rhi::TextureUsage::ColorAttachment;
    voxelDummyTarget_=std::make_unique<rhi::RenderTexture>(device_,target);
    std::vector<const rhi::BindGroupLayout*> clearLayouts={voxelSetLayout_.get()};
    clearPipeline_=std::make_unique<ComputePipeline>(device_,"/shaders/ddgi_clear.comp.wgsl",clearLayouts,sizeof(glm::ivec4));
#else
    d.vertPath=shaderPath("voxelize.vert.spv");d.fragPath=shaderPath("voxelize.frag.spv");
    std::vector<rhi::vulkan::BindGroupLayoutRef> clearLayouts={*voxelSetLayout_};
    clearPipeline_=std::make_unique<ComputePipeline>(device_,shaderPath("ddgi_clear.comp.spv"),clearLayouts,sizeof(glm::ivec4));
#endif
    d.bindGroupLayouts={voxelSetLayout_.get(),&materialLayout};d.depthTest=false;d.depthWrite=false;d.cullMode=rhi::CullMode::None;
    d.pushConstantSize=sizeof(VoxelPush);d.pushConstantStages=rhi::ShaderStages::Vertex|rhi::ShaderStages::Fragment;
    voxelPipeline_=std::make_unique<rhi::Pipeline>(device_,d);
}
void GIVolume::voxelize(rhi::CommandEncoder& e,Scene& scene,GpuProfiler* profiler,const std::vector<std::unique_ptr<ScenePassFeature>>* features) {
    if(!available_ || grid_.bricks().empty()) return;
    SAIDA_GPU_PROFILE_SCOPE(profiler,e.handle(),"DDGI/Voxelize");
    for(auto* t:{voxelTexture_.get(),normalTexture_.get(),emissionTexture_.get()}) e.transition(t->image(),rhi::ResourceState::ShaderRead,rhi::ResourceState::StorageReadWrite);
    auto cp=e.beginComputePass();cp.setPipeline(*clearPipeline_);cp.setBindGroup(0,*voxelSets_[frame_][0]);
    glm::ivec4 push(int(grid_.brickMask().size()),0,0,0);cp.setPushConstants(&push,sizeof(push));cp.dispatch(uint32_t(grid_.bricks().size()));cp.end();e.computeToGraphicsBarrier();
    const auto bricks=grid_.config().bricks();
    std::vector<std::pair<glm::dvec3,glm::dvec3>> boxes;
    for(int k=0;k<desc_.cascades;++k) {
        glm::dvec3 low(1e30),high(-1e30);bool active=false;boxes.clear();
        const auto padding=glm::dvec3(desc_.counts)*grid_.cascade(k).spacing/glm::dvec3(grid_.config().voxels);
        for(uint32_t job:grid_.bricks()) if(int(job)/grid_.config().bricksPerCascade()==k) {
            const auto physical=GIClipmap::coord(int(job)%grid_.config().bricksPerCascade(),bricks)*kGIBrickSize;
            auto first=GIClipmap::wrap(glm::i64vec3(physical-grid_.cascade(k).voxelWrap),grid_.config().voxels);
            auto last=first+glm::ivec3(kGIBrickSize);
            // A physical brick can straddle the toroidal seam. Its world-space
            // union then covers both ends, rather than the gap between them.
            for(int axis=0;axis<3;++axis) if(last[axis]>grid_.config().voxels[axis]) {
                first[axis]=0;last[axis]=grid_.config().voxels[axis];
            }
            const auto size=glm::dvec3(desc_.counts)*grid_.cascade(k).spacing/glm::dvec3(grid_.config().voxels);
            boxes.emplace_back(grid_.cascade(k).origin+glm::dvec3(first)*size-padding,grid_.cascade(k).origin+glm::dvec3(last)*size+padding);
            low=glm::min(low,boxes.back().first);high=glm::max(high,boxes.back().second);
            active=true;
        }
        if(!active) continue;
        // Bricks are scattered (scrolling slabs, invalidated meshes): a mesh is
        // drawn only when it overlaps one of them, not merely their union.
        auto overlaps=[&](const Contributor& c) {
            if(glm::any(glm::lessThan(c.high,low)) || glm::any(glm::greaterThan(c.low,high))) return false;
            for(const auto& box:boxes) if(!glm::any(glm::lessThan(c.high,box.first)) && !glm::any(glm::greaterThan(c.low,box.second))) return true;
            return false;
        };
        rhi::RenderPassDesc pass;pass.width=pass.height=desc_.voxelResolution;
#ifdef SAIDA_RHI_WEBGPU
        pass.colorCount=1;pass.colors[0].view=voxelDummyTarget_->view();pass.colors[0].store=false;
#endif
        auto rp=e.beginRenderPass(pass);rp.setPipeline(*voxelPipeline_);rp.setBindGroup(0,*voxelSets_[frame_][k]);
        for(const auto& item:contributors_) {
            const auto& c=item.second;if(!c.loaded || !overlaps(c)) continue;
            const MeshNode* node=item.first;Mesh* mesh=node->sourceMesh();Material* material=node->material();
            if(!desc_.updateAll && k>1 && node->hasLods()) {for(int lod=int(node->lods().size())-1;lod>0;--lod) if(auto* coarse=node->sourceMesh(lod);coarse && coarse->loaded()) {mesh=coarse;break;}}
            rp.setBindGroup(1,material->descriptorSet());mesh->bind(rp);
            VoxelPush pc{node->worldTransform(),glm::vec4(0,material->desc().foliage.w>0?.4f:1.f,material->desc().doubleSided?1.f:0.f,0)};
            for(int axis=0;axis<3;++axis) {pc.params.x=float(axis);rp.setPushConstants(&pc,sizeof(pc));mesh->draw(rp);}
        }
        rp.end();
        if(features) {
            // Make raster storage writes visible before a feature injects terrain.
            for(auto* texture:{voxelTexture_.get(),normalTexture_.get(),emissionTexture_.get()})
                e.transition(texture->image(),rhi::ResourceState::StorageReadWrite,rhi::ResourceState::StorageReadWrite);
            const GIVoxelContext context{e,uint32_t(frame_),scene,*voxelSetLayout_,*voxelSets_[frame_][k],uint32_t(grid_.bricks().size()),uint32_t(grid_.brickMask().size()),k};
            for(const auto& feature:*features) feature->recordGIVoxels(context);
        }
    }
    for(auto* t:{voxelTexture_.get(),normalTexture_.get(),emissionTexture_.get()}) e.transition(t->image(),rhi::ResourceState::StorageReadWrite,rhi::ResourceState::ShaderRead);
}
void GIVolume::createComputeResources(rhi::BindGroupLayout& globalLayout) {
    raysBuffer_=std::make_unique<Buffer>(device_,uint64_t(grid_.config().maxProbeBudget())*desc_.raysPerProbe*32,rhi::BufferUsage::Storage,MemoryUsage::GpuOnly);
#ifdef SAIDA_RHI_WEBGPU
    std::vector<rhi::webgpu::BindGroupLayoutEntry> entries;
    for(int b:{0,5}) {rhi::webgpu::BindGroupLayoutEntry e{};e.binding=b;e.type=rhi::BindingType::StorageBuffer;e.visibility=rhi::ShaderStages::Compute;entries.push_back(e);}
    for(int b:{1,2}) {rhi::webgpu::BindGroupLayoutEntry e{};e.binding=b;e.type=rhi::BindingType::StorageImage;e.visibility=rhi::ShaderStages::Compute;e.storageFormat=kAtlasFormat;e.storageAccess=WGPUStorageTextureAccess_WriteOnly;entries.push_back(e);}
    for(int b:{3,4,6,7}) {rhi::webgpu::BindGroupLayoutEntry e{};e.binding=b;e.type=rhi::BindingType::SampledTexture;e.visibility=rhi::ShaderStages::Compute;e.dim=b>=6?rhi::webgpu::TextureDim::Dim3D:rhi::webgpu::TextureDim::Dim2D;entries.push_back(e);}
    for(int b=8;b<12;++b) {rhi::webgpu::BindGroupLayoutEntry e{};e.binding=b;e.type=rhi::BindingType::Sampler;e.visibility=rhi::ShaderStages::Compute;entries.push_back(e);}
#else
    std::vector<rhi::BindGroupLayoutEntry> entries;
    for(int b:{0,5}) entries.push_back({uint32_t(b),rhi::BindingType::StorageBuffer,rhi::ShaderStages::Compute});
    for(int b:{1,2}) entries.push_back({uint32_t(b),rhi::BindingType::StorageImage,rhi::ShaderStages::Compute});
    for(int b:{3,4,6,7}) entries.push_back({uint32_t(b),rhi::BindingType::CombinedImageSampler,rhi::ShaderStages::Compute});
#endif
    giComputeSetLayout_=std::make_unique<rhi::BindGroupLayout>(device_,entries);
    for(int f=0;f<2;++f)for(int mode=0;mode<2;++mode) {
        auto irr=imageEntry(3,irradiance_[mode]->view(),true);
        auto vis=imageEntry(4,visibility_[mode]->view(),true);
        auto normal=imageEntry(6,normalTexture_->view());
        auto emission=imageEntry(7,emissionTexture_->view());
#ifndef SAIDA_RHI_WEBGPU
        // Combined image samplers. WebGPU binds the samplers apart (8-11): a
        // sampler on a texture entry makes Dawn read it as a sampler entry.
        for(auto* e:{&irr,&vis,&normal,&emission}) e->sampler=sampler_->handle();
#endif
        std::vector<rhi::BindGroupEntry> bindings={bufferEntry(0,*raysBuffer_),imageEntry(1,irradiance_[1-mode]->view(),true),imageEntry(2,visibility_[1-mode]->view(),true),irr,vis,bufferEntry(5,*schedule_[f]),normal,emission};
#ifdef SAIDA_RHI_WEBGPU
        for(int b=8;b<12;++b) {rhi::BindGroupEntry s{};s.binding=b;s.sampler=sampler_->handle();bindings.push_back(s);}
#endif
        giComputeSets_[f][mode]=std::make_unique<rhi::BindGroup>(*giComputeSetLayout_,bindings);
    }
#ifdef SAIDA_RHI_WEBGPU
    std::vector<const rhi::BindGroupLayout*> layouts={&globalLayout,giComputeSetLayout_.get()};
    auto path=[](const char* name){return std::string("/shaders/")+name+".wgsl";};
#else
    std::vector<rhi::vulkan::BindGroupLayoutRef> layouts={globalLayout,*giComputeSetLayout_};
    auto path=[](const char* name){return shaderPath(std::string(name)+".spv");};
#endif
    tracePipeline_=std::make_unique<ComputePipeline>(device_,path("ddgi_trace.comp"),layouts,sizeof(TracePush));
    blendPipeline_=std::make_unique<ComputePipeline>(device_,path("ddgi_blend.comp"),layouts,sizeof(BlendPush));
    scatterPipeline_=std::make_unique<ComputePipeline>(device_,path("ddgi_scatter.comp"),layouts,sizeof(ScatterPush));
}
void GIVolume::update(rhi::CommandEncoder& e,const rhi::BindGroup& global,GpuProfiler* profiler,const rhi::BindGroup* scatterGlobal) {
    if(!available_) return;
    const auto& scatter=scatterGlobal?*scatterGlobal:global;
    e.graphicsToComputeBarrier();
    auto scatterPass=[&](int mode,int count,int offset) {
        if(!count) return;
        auto cp=e.beginComputePass();cp.setPipeline(*scatterPipeline_);cp.setBindGroup(0,scatter);cp.setBindGroup(1,*giComputeSets_[frame_][1]);
        ScatterPush pc{mode,count,offset};cp.setPushConstants(&pc,sizeof(pc));cp.dispatch((std::max(desc_.irradianceTexels,desc_.visibilityTexels)+9)/8,(std::max(desc_.irradianceTexels,desc_.visibilityTexels)+9)/8,count);cp.end();
    };
    scatterPass(1,resetCount_,grid_.config().maxProbeBudget());e.storageBarrier();
    if(probeCount_) {
        {
            SAIDA_GPU_PROFILE_SCOPE(profiler,e.handle(),"DDGI/Trace");
            auto cp=e.beginComputePass();cp.setPipeline(*tracePipeline_);cp.setBindGroup(0,global);cp.setBindGroup(1,*giComputeSets_[frame_][0]);
            TracePush p{desc_.raysPerProbe,probeCount_};cp.setPushConstants(&p,sizeof(p));cp.dispatch((probeCount_*desc_.raysPerProbe+63)/64);cp.end();
        }
        e.storageBarrier();
        {
            SAIDA_GPU_PROFILE_SCOPE(profiler,e.handle(),"DDGI/Blend");
            auto cp=e.beginComputePass();cp.setPipeline(*blendPipeline_);cp.setBindGroup(0,global);cp.setBindGroup(1,*giComputeSets_[frame_][0]);
            for(int mode=0;mode<2;++mode) {int t=(mode?desc_.visibilityTexels:desc_.irradianceTexels)+2;BlendPush p{mode,desc_.raysPerProbe,probeCount_,desc_.hysteresis,desc_.distExponent};cp.setPushConstants(&p,sizeof(p));cp.dispatch((t+7)/8,(t+7)/8,probeCount_);}cp.end();
        }
        e.storageBarrier();scatterPass(0,probeCount_,0);
    }
    e.computeToGraphicsBarrier();
}
}
