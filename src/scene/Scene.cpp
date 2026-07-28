#include "scene/Scene.hpp"
#include "scene/Behaviour.hpp"
#include "core/Profiler.hpp"
#include "nodes/MeshNode.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/WaterNode.hpp"  // water renders on web too (BeachDemo)
#include "nodes/UICanvasNode.hpp"
#ifndef SAIDA_RHI_WEBGPU
#include "nodes/WebCanvasNode.hpp"
#endif
#include "nodes/ParticleSystemNode.hpp"
#include "scene/SerializationHelpers.hpp"
#include "scene/SceneSettingsSerialization.hpp"
#include "graphics/ResourceManager.hpp"
#ifndef SAIDA_NO_PHYSICS
#include "physics/PhysicsWorld.hpp"
#include "physics/CollisionObjectNode.hpp"
#include "physics/AreaNode.hpp"
#include "physics/JointNodes.hpp"
#endif

#include <nlohmann/json.hpp>

namespace saida {

Scene::Scene() : Node("Scene") {
}

Scene::~Scene() {
    // Destroy the node tree (and thus every physics body) while the PhysicsWorld
    // is still alive — the Node base subobject (which owns children_) is otherwise
    // torn down after this Scene's members, leaving body destructors with a
    // dangling world pointer.
    clearChildren();
}

void Scene::update(float dt) {
    SAIDA_PROFILE_FUNCTION();
    if (lastHierarchyVersion_ != g_hierarchyVersion) {
        SAIDA_PROFILE_SCOPE("Scene/FlattenHierarchy");
        flattenHierarchy();
        lastHierarchyVersion_ = g_hierarchyVersion;
    }

    SAIDA_PROFILE_COUNTER("Scene/Behaviours", flatBehaviours_.size());
    SAIDA_PROFILE_COUNTER("Scene/Nodes", activeNodeCount_);
    SAIDA_PROFILE_COUNTER("Scene/MeshNodes", meshes_.size());
    SAIDA_PROFILE_COUNTER("Scene/Lights", lights_.size());
    SAIDA_PROFILE_COUNTER("Physics/Bodies", bodies_.size());

    {
        SAIDA_PROFILE_SCOPE("Scene/Behaviours");
        for (auto* b : flatBehaviours_) {
            if (!b->enabled()) continue;
            if (dt > 0.0f) {
                if (!b->ready_) {
                    b->onReady();
                    b->ready_ = true;
                }
                b->onUpdate(dt);
            }
        }
    }

    {
        SAIDA_PROFILE_SCOPE("Scene/UpdateTransforms");
        updateTransforms(glm::mat4(1.0f), false);
    }

#ifndef SAIDA_NO_PHYSICS
    // Freeze each Auto collision shape once, now that world transforms are fresh
    // (runs in edit mode too, so the editor wireframe is stable and correct).
    {
        SAIDA_PROFILE_SCOPE("Physics/ResolveAutoShapes");
        for (auto* body : bodies_) body->resolveAutoShapes();
    }

    // Physics only runs while time is advancing (i.e. in Play, not while editing).
    if (dt > 0.0f && !bodies_.empty()) {
        SAIDA_PROFILE_SCOPE("Physics/SceneStep");
        if (!physics_) physics_ = std::make_unique<PhysicsWorld>();
        {
            SAIDA_PROFILE_SCOPE("Physics/SyncTo");
            for (auto* body : bodies_) body->syncToPhysics(*physics_);
        }
        {
            // After body sync so both referenced bodies exist; a joint whose
            // body was rebuilt this frame recreates its constraint here.
            SAIDA_PROFILE_SCOPE("Physics/SyncJoints");
            for (auto* joint : joints_) joint->syncJointToPhysics(*physics_);
        }
        {
            SAIDA_PROFILE_SCOPE("Physics/PreStep");
            for (auto* body : bodies_) body->prePhysicsStep(*physics_, dt);  // characters move/slide
        }
        physics_->step(dt);
        {
            SAIDA_PROFILE_SCOPE("Physics/SyncFrom");
            for (auto* body : bodies_) body->syncFromPhysics(*physics_);
        }

        // Dispatch contact events on the main thread: sensor overlaps to Area
        // nodes, solid collisions to both bodies' collision signals.
        {
        SAIDA_PROFILE_SCOPE("Physics/ContactEvents");
        for (const auto& e : physics_->drainContactEvents()) {
            auto* n1 = static_cast<CollisionObjectNode*>(physics_->bodyUserData(e.a));
            auto* n2 = static_cast<CollisionObjectNode*>(physics_->bodyUserData(e.b));
            if (e.sensor) {
                if (auto* area = dynamic_cast<AreaNode*>(n1)) area->handleOverlap(n2, e.entered);
                if (auto* area = dynamic_cast<AreaNode*>(n2)) area->handleOverlap(n1, e.entered);
            } else {
                if (n1) (e.entered ? n1->collisionEntered : n1->collisionExited).emit(n2);
                if (n2) (e.entered ? n2->collisionEntered : n2->collisionExited).emit(n1);
            }
        }
        }

        {
            SAIDA_PROFILE_SCOPE("Scene/PostPhysicsTransforms");
            updateTransforms(glm::mat4(1.0f), false);  // propagate dynamic results down the tree
        }
    }
#endif
}

void Scene::refreshHierarchy() {
    if (lastHierarchyVersion_ != g_hierarchyVersion) {
        flattenHierarchy();
        lastHierarchyVersion_ = g_hierarchyVersion;
    }
}

void Scene::flattenHierarchy() {
    meshes_.clear();
    lights_.clear();
    uiCanvas_ = nullptr;
    webCanvases_.clear();
    waterNodes_.clear();
    particleSystems_.clear();
    flatBehaviours_.clear();
    bodies_.clear();
    joints_.clear();
    activeNodeCount_ = 0;

    traverse([this](Node& n, const glm::mat4&) {
        if (!n.isActiveInHierarchy()) return;
        ++activeNodeCount_;

        if (MeshNode* mn = dynamic_cast<MeshNode*>(&n)) {
            if (mn->meshEnabled()) {
                meshes_.push_back(mn);
            }
        }
        if (n.asLight()) {
            lights_.push_back(static_cast<LightNode*>(&n));
        }
        if (auto* water = dynamic_cast<WaterNode*>(&n)) {
            waterNodes_.push_back(water);
        }
        if (!uiCanvas_) {
            if (auto* canvas = dynamic_cast<UICanvasNode*>(&n)) {
                uiCanvas_ = canvas;
            }
        }
#ifndef SAIDA_RHI_WEBGPU
        if (auto* webCanvas = dynamic_cast<WebCanvasNode*>(&n)) {
            webCanvases_.push_back(webCanvas);
        }
#endif
        if (auto* ps = dynamic_cast<ParticleSystemNode*>(&n)) {
            particleSystems_.push_back(ps);
        }
        if (CollisionObjectNode* co = n.asCollisionObject()) {
            bodies_.push_back(co);
        }
        if (JointNode* joint = n.asJointNode()) {
            joints_.push_back(joint);
        }
        for (auto& b : n.behaviours()) {
            flatBehaviours_.push_back(b.get());
        }
    });
}

void Scene::serialize(nlohmann::json& j, ResourceManager& resources) const {
    Node::serialize(j, resources);
    if (prefabAssetId_ != kAssetInvalid) {
        j["prefabAssetId"] = prefabAssetId_;
        j.erase("children"); // Do not serialize children for prefabs
    }
    writeSceneSettings(settings_, j["settings"]);

    if (!connectionDefs_.empty()) {
        nlohmann::json conns = nlohmann::json::array();
        for (const auto& c : connectionDefs_)
            conns.push_back({{"from", c.from}, {"signal", c.signal},
                             {"to", c.to}, {"slot", c.slot}});
        j["connections"] = std::move(conns);
    }
}

void Scene::readConnections(const nlohmann::json& j) {
    connectionDefs_.clear();
    auto it = j.find("connections");
    if (it == j.end() || !it->is_array()) return;
    for (const auto& cj : *it) {
        SignalConnectionDef def;
        def.from = cj.value("from", kNodeInvalid);
        def.signal = cj.value("signal", std::string{});
        def.to = cj.value("to", kNodeInvalid);
        def.slot = cj.value("slot", std::string{});
        if (def.from != kNodeInvalid && def.to != kNodeInvalid &&
            !def.signal.empty() && !def.slot.empty())
            connectionDefs_.push_back(std::move(def));
    }
}

void Scene::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    Node::deserialize(j, resources);
    readConnections(j);
    if (j.contains("prefabAssetId")) {
        prefabAssetId_ = j["prefabAssetId"].get<AssetID>();
    }
    // Patch semantics: a prefab instance overrides only the settings it names.
    if (auto it = j.find("settings"); it != j.end())
        applySceneSettings(*it, settings_, resources);
}

} // namespace saida
