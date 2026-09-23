#pragma once

#include "scene/Node.hpp"
#include "scene/SceneIndex.hpp"
#include "scene/SignalWiring.hpp"
#include "core/ReflectionFwd.hpp"
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

    // A second sky the first one fades into: the drawn sky is
    // mix(skyboxTexture, skyboxBlendTexture, skyboxBlend), each image turned by
    // its own rotation so two photographs whose Sun sits at different azimuths
    // can both be aligned on one light. A day/night cycle walks through a set of
    // skies by moving the pair and the blend. Only the sky pass reads it; IBL and
    // the reflection environment keep sampling `skyboxTexture` alone.
    AssetID skyboxBlendTexture = kAssetInvalid;
    float skyboxBlend = 0.0f;
    float skyboxBlendRotation = 0.0f;

    // A Sun disc drawn over the sky, toward `skySunDirection` (world space,
    // pointing at the Sun), `skySunSize` degrees in angular radius and of
    // linear radiance `skySunColor`. Black draws nothing. It exists for skies
    // whose photographs had their own Sun removed: two photographs crossfading
    // would otherwise show two Suns, neither where the light comes from.
    glm::vec3 skySunDirection{0.0f, 1.0f, 0.0f};
    glm::vec3 skySunColor{0.0f};
    float skySunSize = 0.265f;

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

    // The reflected view of the durable environment, in `SceneSettingsReflection.cpp`.
    //
    // It describes exactly the fields `writeSceneSettings` persists — the
    // authored environment — and deliberately not the transient bake state
    // (`baked`, `bakeRequested`) or the editor's debug toggles
    // (`giDebugVoxels`, `showSkeletons`), which are engine state a scene never
    // carries and no author should be able to write. `SceneSettingsTests` pins
    // that equivalence in both directions, so a field added here and forgotten
    // in one of the two lists fails the build's tests rather than going
    // silently unsaved or unwritable.
    //
    // Consumers: the `set_scene_setting` op and the `scene.setSetting` script
    // binding both resolve through this and nothing else. The Inspector and the
    // MCP `set_scene_settings` tool still carry their own key lists.
    static void describe(reflect::TypeBuilder<SceneSettings>& t);
};

// The description above, built once. Every writer that resolves a setting by
// name goes through this; see `SceneSettingsReflection.cpp` for which do not.
const reflect::TypeDesc& sceneSettingsDesc();

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

    // Updates active behaviours, transforms and fixed-step physics.
    void update(float dt);

    SceneSettings& settings() { return settings_; }
    const SceneSettings& settings() const { return settings_; }

    // Refreshes membership and resource ownership in changed branches since
    // the last update. Must be called after deferred mutations (queueFree,
    // changeScene): the same frame's rendering must never see pointers to
    // destroyed nodes.
    void refreshHierarchy();
    void updateRenderLods(const glm::mat4& view, const glm::mat4& projection);
    // old world point p becomes rotation*p + translation. Uniformly scaled
    // parents only; body identities/velocities survive, joint anchors rebuild.
    // Call between updates, and rebase any game-owned world-space state too.
    void rebaseSubtree(Node& root, const glm::vec3& translation,
                       const glm::quat& rotation = glm::quat(1.f, 0.f, 0.f, 0.f));
    void rebaseOrigin(const glm::vec3& translation,
                      const glm::quat& rotation = glm::quat(1.f, 0.f, 0.f, 0.f));
    const ResourceManager::AssetUsage& resourceUsage() const { return index_.resources(); }
    uint64_t resourceVersion() const { return index_.resourceVersion(); }
    bool containsNode(const Node* node) const { return index_.contains(node); }
    size_t indexedNodesLastRefresh() const { return index_.visitedNodes(); }
    uint64_t indexedNodesTotal() const { return index_.totalVisitedNodes(); }

    const std::vector<MeshNode*>& meshes() const { return index_.meshes.values(); }
    const std::vector<LightNode*>& lights() const { return index_.lights.values(); }
    UICanvasNode* uiCanvas() const { return index_.canvases.values().empty() ? nullptr : index_.canvases.values().front(); }
    const std::vector<WebCanvasNode*>& webCanvases() const { return index_.webCanvases.values(); }
    const std::vector<WaterNode*>& waterNodes() const { return index_.water.values(); }
    const std::vector<ParticleSystemNode*>& particleSystems() const { return index_.particles.values(); }

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
    SceneIndex index_;

    SceneSettings settings_;
    std::vector<SignalConnectionDef> connectionDefs_;
    SignalWiring wiring_;
    AssetID prefabAssetId_ = kAssetInvalid;

#ifndef SAIDA_NO_PHYSICS
    std::unique_ptr<PhysicsWorld> physics_;
    // Raw Jolt body ids awake during this frame's substeps (kept to avoid a
    // per-frame allocation, and raw so this header stays free of Jolt).
    std::vector<uint32_t> movedBodies_;
#endif
    SceneTree* tree_ = nullptr;
};

} // namespace saida
