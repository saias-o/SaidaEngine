#pragma once

#include "scene/Node.hpp"
#include "scene/SignalWiring.hpp"
#include "project/AssetRegistry.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace saida {

class PhysicsWorld;
class CollisionObjectNode;
class JointNode;

// Whether lighting is evaluated live every frame, or frozen from a bake.
enum class LightingMode {
    Realtime,  // shadows/lighting recomputed each frame; baking disabled
    Baked,     // DDGI volume converged then frozen via "Bake GI"
};

enum class GIMode {
    FullRealtime,       // DDGI updates every frame; best responsiveness.
    AmortizedRealtime,  // DDGI warms up, then updates on a cadence; better FPS.
};

struct SceneSettings {
    glm::vec4 ambientLight{0.1f, 0.1f, 0.1f, 1.0f};
    glm::vec4 clearColor{0.0f, 0.0f, 0.0f, 1.0f};
    bool enablePostProcessing = true;

    LightingMode lightingMode = LightingMode::Realtime;
    bool baked = false;          // true once the DDGI bake/freeze has converged
    bool bakeRequested = false;  // transient: editor asks the renderer to bake GI

    // Global illumination (DDGI irradiance volume — the single GI primitive).
    bool giEnabled = true;       // sample the irradiance volume for indirect diffuse
    GIMode giMode = GIMode::FullRealtime;
    bool giDebugVoxels = false;  // debug: visualize the voxelized scene albedo
    float giIntensity = 1.0f;    // indirect diffuse multiplier (boost to make GI visible)

    AssetID skyboxTexture = kAssetInvalid;
    float skyboxExposure = 1.0f;
    float skyboxRotation = 0.0f;

    // Image-based lighting samples the same equirectangular environment as the
    // skybox. The shader is shared by desktop/XR/mobile; platforms may only vary
    // texture resolution, not the lighting equation.
    bool iblEnabled = true;
    float iblDiffuseIntensity = 0.35f;
    float iblSpecularIntensity = 1.0f;

    // Canonical screen-space AO. Realtime and Baked lighting both use this same
    // pass; only buffer resolution may vary by platform.
    bool aoEnabled = true;
    float aoRadius = 0.75f;
    float aoIntensity = 1.0f;
    float aoPower = 1.35f;

    bool fogEnabled = false;
    glm::vec4 fogColor{0.55f, 0.62f, 0.72f, 1.0f};
    float fogStart = 8.0f;
    float fogDensity = 0.035f;

    bool bloomEnabled = true;
    float bloomThreshold = 1.0f;
    float bloomIntensity = 0.25f;
    float bloomRadius = 3.0f;

    // Debug: draw animated skeletons as bone lines (editor tool; desktop only).
    bool showSkeletons = false;

    // When a sub-scene with this flag is loaded into the persistent World, its
    // rendering settings (above) override the World's. If false, the World keeps
    // the settings it already has (those of the "super-scene" / a previous level).
    bool changeRenderingAtLoad = true;
};

class MeshNode;
class LightNode;
class UICanvasNode;
class WebCanvasNode;
class WaterNode;
class ParticleSystemNode;
class SceneTree;

// A Scene is simply the root Node of a tree (cf. Godot: a scene is a node).

class Scene : public Node {
public:
    Scene();
    ~Scene() override;

    const char* typeName() const override { return "Scene"; }
    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;

    // Invokes updateTree on the root, recursively updating all behaviours.
    void update(float dt);

    SceneSettings& settings() { return settings_; }
    const SceneSettings& settings() const { return settings_; }

    // Re-flattens the caches (meshes/lights/...) if the tree has changed since
    // the last update. Must be called after deferred mutations (queueFree,
    // changeScene): the same frame's rendering must never see pointers to
    // destroyed nodes.
    void refreshHierarchy();

    const std::vector<MeshNode*>& meshes() const { return meshes_; }
    const std::vector<LightNode*>& lights() const { return lights_; }
    UICanvasNode* uiCanvas() const { return uiCanvas_; }
    const std::vector<WebCanvasNode*>& webCanvases() const { return webCanvases_; }
    const std::vector<WaterNode*>& waterNodes() const { return waterNodes_; }
    const std::vector<ParticleSystemNode*>& particleSystems() const { return particleSystems_; }

    // The per-scene physics world (created lazily once a body exists; null until then).
    PhysicsWorld* physics() const {
#ifdef SAIDA_NO_PHYSICS
        return nullptr;
#else
        return physics_.get();
#endif
    }

    // SceneTree wiring: only the persistent World root carries a tree pointer;
    // Node::tree() walks to the root and reads it via ownTree().
    void setTree(SceneTree* t) { tree_ = t; }
    SceneTree* ownTree() const override { return tree_; }

    AssetID prefabAssetId() const { return prefabAssetId_; }
    void setPrefabAssetId(AssetID id) { prefabAssetId_ = id; }

    // Data-driven signal→slot links (serialized in the scene's "connections").
    std::vector<SignalConnectionDef>& connections() { return connectionDefs_; }
    const std::vector<SignalConnectionDef>& connections() const { return connectionDefs_; }

    // Wire (or rewire) the declared connections against this scene. Called at Play
    // when the scene becomes the current sub-scene; a no-op when there are none.
    void applyConnections() { wiring_.apply(*this, connectionDefs_); }
    void clearConnections() { wiring_.clear(); }

    // Parse a scene-root JSON's "connections" array into connectionDefs_ (used by
    // both Scene::deserialize and the SceneSerializer's manual root-load path).
    void readConnections(const nlohmann::json& j);

private:
    void flattenHierarchy();

    SceneSettings settings_;
    std::vector<SignalConnectionDef> connectionDefs_;
    SignalWiring wiring_;
    AssetID prefabAssetId_ = kAssetInvalid;
    uint32_t lastHierarchyVersion_ = 0;

    std::vector<MeshNode*> meshes_;
    std::vector<LightNode*> lights_;
    UICanvasNode* uiCanvas_ = nullptr;
    std::vector<WebCanvasNode*> webCanvases_;
    std::vector<WaterNode*> waterNodes_;
    std::vector<ParticleSystemNode*> particleSystems_;
    std::vector<Behaviour*> flatBehaviours_;
    std::vector<CollisionObjectNode*> bodies_;
    std::vector<JointNode*> joints_;
    uint32_t activeNodeCount_ = 0;

#ifndef SAIDA_NO_PHYSICS
    std::unique_ptr<PhysicsWorld> physics_;
#endif
    SceneTree* tree_ = nullptr;
};

} // namespace saida
