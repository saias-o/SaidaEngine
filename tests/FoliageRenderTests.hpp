#pragma once
#include "WeatherRenderTestSupport.hpp"
#include "graphics/Material.hpp"
#include "graphics/ResourceManager.hpp"
#include "graphics/Mesh.hpp"
#include "nodes/MeshNode.hpp"
#include "nodes/LightNode.hpp"
#include "scene/GLTFLoader.hpp"
#include "scene/Scene.hpp"
#include "render/FrustumCulling.hpp"
#include <unordered_set>

namespace saida::weather_test {
inline SceneSetup foliageFixture(bool objectNormals, float transmission, float wind, bool backlight, glm::vec3 translation = glm::vec3(0)) {
    return [=](Scene& scene, ResourceManager& resources) {
        auto& s = scene.settings();
        s.giEnabled = s.iblEnabled = s.bloomEnabled = s.aoEnabled = s.fogEnabled = false;
        s.ambientLight = glm::vec4(.025f, .025f, .025f, 1);
        s.clearColor = glm::vec4(.015f, .02f, .03f, 1);
        MaterialDesc d;
        d.baseColor = {.18f,.30f,.09f,1}; d.doubleSided = true; d.alphaCutoff = .4f;
        d.foliage = {.35f, transmission, wind, objectNormals ? 1.f : .5f};
        const uint8_t normals[] = {30,175,200,230, 220,175,200,80};
        d.normalId = resources.registerGeneratedTexture(normals,2,1,rhi::Format::RGBA8Unorm,false);
        uint8_t alpha[16*16*4];
        for (int y=0;y<16;++y) for(int x=0;x<16;++x) {
            const int i=(y*16+x)*4;
            alpha[i]=alpha[i+1]=alpha[i+2]=255;
            alpha[i+3]=((x-8)*(x-8)+(y-8)*(y-8)<61)?255:0;
        }
        d.albedoId = resources.registerGeneratedTexture(alpha,16,16,rhi::Format::RGBA8Unorm,true);
        std::vector<Vertex> v(4);
        const glm::vec3 p[]={{-1,0,0},{1,0,0},{1,2,0},{-1,2,0}};
        const glm::vec2 uv[]={{0,1},{1,1},{1,0},{0,0}};
        for(int i=0;i<4;++i) { v[i].pos=p[i];v[i].normal={0,0,1};v[i].color=glm::vec3(1);v[i].texCoord=uv[i];v[i].tangent={1,0,0,1}; }
        const auto mesh=resources.registerMemoryMesh(v,{0,1,2,0,2,3});
        auto* node=scene.createChild<MeshNode>("Foliage receiver",resources.getMesh(mesh),resources.getMaterial(d));
        node->transform().position=translation;
        auto* sun=scene.createChild<LightNode>();sun->castShadows=false;sun->intensity=4;
        sun->direction=glm::normalize(backlight?glm::vec3(0,-.1f,1):glm::vec3(.8f,-.3f,-.6f));
    };
}

inline void runFoliageGpuTests(const std::filesystem::path& out) {
    std::unordered_set<MaterialDesc> variants;
    MaterialDesc d;variants.insert(d);
    require(d.foliage==glm::vec4(0),"foliage is opt-in");
    for(int component=0;component<4;++component) { auto changed=d;changed.foliage[component]=.1f;variants.insert(changed); }
    require(variants.size()==5,"material cache keeps every foliage response distinct");
    for(int t=0;t<100;++t) {
        const float phase=float(t)*.37f;
        const float gust=std::sin(phase*1.35f)+.35f*std::sin(phase*3.1f);
        const glm::vec3 displacement=glm::vec3(1,0,.43f)*gust*1.2f*1.2f*.25f;
        require(glm::length(displacement)<=foliageWindMargin({0,0,.25f,1}),"foliage culling bounds enclose the strongest sway");
    }
    const glm::vec3 eye(0,1,4), target(0,1,0);
    const auto mapped=capture(out/"foliage-object-normals.png",foliageFixture(true,.16f,0,false),eye,target);
    const auto tangent=capture(out/"foliage-tangent-normals.png",foliageFixture(false,.16f,0,false),eye,target);
    require(compare(mapped,tangent).meanDifference>.001,"object-space normals and visibility affect lighting");
    const auto opaqueBack=capture(out/"foliage-backlight-opaque.png",foliageFixture(true,0,0,true),eye,target);
    const auto leafBack=capture(out/"foliage-backlight-leaf.png",foliageFixture(true,.35f,0,true),eye,target);
    require(compare(opaqueBack,leafBack).meanB>compare(opaqueBack,leafBack).meanA+.005,"leaf transmission responds to backlighting");
    const auto windA=capture(out/"foliage-wind-a.png",foliageFixture(true,.16f,.18f,false),eye,target,0);
    const auto windB=capture(out/"foliage-wind-b.png",foliageFixture(true,.16f,.18f,false),eye,target,2.2f);
    require(compare(windA,windB).changedFraction>.005,"vertex wind animates cutout silhouette");
    const auto stillA=capture(out/"foliage-still-a.png",foliageFixture(true,.16f,0,false),eye,target,0);
    const auto stillB=capture(out/"foliage-still-b.png",foliageFixture(true,.16f,0,false),eye,target,2.2f);
    require(compare(stillA,stillB).maxDifference==0,"zero sway is pixel-stable over time");
    const auto rebased=capture(out/"foliage-rebased.png",foliageFixture(true,.16f,.18f,false,{350,0,0}),
        eye+glm::vec3(350,0,0),target+glm::vec3(350,0,0),2.2f);
    require(compare(windB,rebased).meanDifference<.0001,"floating-origin translation preserves colour and wind phase");
}

inline void captureFoliageAsset(const std::filesystem::path& model, const std::filesystem::path& out) {
    auto setup=[=](Scene& scene,ResourceManager& resources) {
        auto& s=scene.settings();
        s.giEnabled=s.iblEnabled=s.bloomEnabled=s.aoEnabled=s.fogEnabled=false;
        s.ambientLight=glm::vec4(.12f,.14f,.16f,1);
        s.clearColor=glm::vec4(.14f,.17f,.21f,1);
        auto root=std::make_unique<Node>("Photographed foliage");
        require(GLTFLoader::load(model.generic_string(),*root,resources),"foliage glTF loads through the engine");
        size_t foliageMaterials=0;
        root->traverse([&](Node& n,const glm::mat4&) {
            if (!n.material() || n.material()->desc().foliage.w<=0.f) return;
            ++foliageMaterials;
            require(n.material()->desc().normalId!=kAssetInvalid,"foliage object normals survive import without UV tangents");
        });
        require(foliageMaterials>0,"asset uses the generic foliage material");
        scene.addChild(std::move(root));
        auto* sun=scene.createChild<LightNode>();sun->intensity=3;sun->castShadows=false;
        sun->direction=glm::normalize(glm::vec3(.5f,-.8f,-.3f));
    };
    for(int view=0;view<4;++view) {
        const float a=float(view)*1.57079632679f+.35f;
        capture(out/("view-"+std::to_string(view)+".png"),setup,
            {std::sin(a)*1.9f,1.0f,std::cos(a)*1.9f},{0,.45f,0},0);
    }
}
}
