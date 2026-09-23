#include "scene/Scene.hpp"
#include "scene/Behaviour.hpp"
#include "behaviours/LODGroupBehaviour.hpp"
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
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>
#include <algorithm>
#include <cmath>

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
    refreshHierarchy();

    SAIDA_PROFILE_COUNTER("Scene/Behaviours", index_.behaviours.values().size());
    SAIDA_PROFILE_COUNTER("Scene/Nodes", index_.activeNodes);
    SAIDA_PROFILE_COUNTER("Scene/MeshNodes", index_.meshes.values().size());
    SAIDA_PROFILE_COUNTER("Scene/Lights", index_.lights.values().size());
    SAIDA_PROFILE_COUNTER("Physics/Bodies", index_.bodies.values().size());

    {
        SAIDA_PROFILE_SCOPE("Scene/Behaviours");
        for (auto* b : index_.behaviours.values()) {
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
        for (auto* body : index_.bodies.values()) body->resolveAutoShapes();
    }

    // Physics only runs while time is advancing (i.e. in Play, not while editing).
    if (dt > 0.0f && !index_.bodies.values().empty()) {
        SAIDA_PROFILE_SCOPE("Physics/SceneStep");
        if (!physics_) physics_ = std::make_unique<PhysicsWorld>();
        {
            SAIDA_PROFILE_SCOPE("Physics/SyncTo");
            for (auto* body : index_.bodies.values()) body->syncToPhysics(*physics_);
        }
        {
            // After body sync so both referenced bodies exist; a joint whose
            // body was rebuilt this frame recreates its constraint here.
            SAIDA_PROFILE_SCOPE("Physics/SyncJoints");
            for (auto* joint : index_.joints.values()) joint->syncJointToPhysics(*physics_);
        }
        // Per substep, only what asked to be called. Node transforms are not
        // touched between substeps: `onPhysicsStep` reads the body from Jolt,
        // and the tree is brought up to date once, after the last substep.
        movedBodies_.clear();
        physics_->step(dt, [&](float fixedDt) {
            for (auto* b : index_.physicsSteppers.values())
                if (b->enabled() && b->ready_) b->onPhysicsStep(fixedDt);
            for (auto* body : index_.preSteppers.values()) body->prePhysicsStep(*physics_, fixedDt);
        }, [&](float) {
            // A body that was awake during any substep moved this frame, even
            // if it has fallen asleep since; sleeping and static bodies did not.
            physics_->appendActiveBodies(movedBodies_);
        });
        {
            SAIDA_PROFILE_SCOPE("Physics/SyncFrom");
            for (auto* body : index_.preSteppers.values()) body->syncFromPhysics(*physics_);
            std::sort(movedBodies_.begin(), movedBodies_.end());
            movedBodies_.erase(std::unique(movedBodies_.begin(), movedBodies_.end()), movedBodies_.end());
            for (uint32_t id : movedBodies_)
                if (auto* body = static_cast<CollisionObjectNode*>(physics_->bodyUserData(JPH::BodyID(id))))
                    body->syncFromPhysics(*physics_);
        }
        {
            SAIDA_PROFILE_SCOPE("Scene/PostPhysicsTransforms");
            updateTransforms(glm::mat4(1.0f), false);  // propagate dynamic results down the tree
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


    }
#endif
}

void Scene::refreshHierarchy() {
    index_.refresh(*this);
}

void Scene::updateRenderLods(const glm::mat4& view, const glm::mat4& projection) {
    for (auto* group : index_.lodGroups.values())
        if (group->enabled()) group->updateForView(view, projection);
    refreshHierarchy();
}

void Scene::rebaseSubtree(Node& root, const glm::vec3& translation, const glm::quat& rotation) {
    const Node* ancestor = &root;
    while (ancestor && ancestor != this) ancestor = ancestor->parent();
    if (!ancestor || &root == this) throw std::invalid_argument("rebaseSubtree requires a descendant of this scene");
    if (!std::isfinite(translation.x) || !std::isfinite(translation.y) || !std::isfinite(translation.z) ||
        !std::isfinite(glm::dot(rotation, rotation)) || std::abs(glm::dot(rotation, rotation) - 1.f) > 1e-4f)
        throw std::invalid_argument("rebase requires a finite translation and unit quaternion");
    // Read the live parent chain; callers need not have stepped the scene first.
    glm::mat4 parentWorld(1.f);
    std::vector<const Node*> parents;
    for (auto* p = root.parent(); p; p = p->parent()) parents.push_back(p);
    for (auto it = parents.rbegin(); it != parents.rend(); ++it) parentWorld *= (*it)->localMatrix();
    const glm::vec3 scale{glm::length(glm::vec3(parentWorld[0])), glm::length(glm::vec3(parentWorld[1])),
                          glm::length(glm::vec3(parentWorld[2]))};
    if (scale.x < 1e-6f || std::abs(scale.x - scale.y) > 1e-5f || std::abs(scale.x - scale.z) > 1e-5f ||
        glm::determinant(glm::mat3(parentWorld)) <= 0.f)
        throw std::invalid_argument("rebase requires a parent frame with positive uniform scale");
    const glm::mat3 frame = glm::mat3(parentWorld) / scale.x;
    if (std::abs(glm::dot(frame[0], frame[1])) > 1e-5f ||
        std::abs(glm::dot(frame[0], frame[2])) > 1e-5f ||
        std::abs(glm::dot(frame[1], frame[2])) > 1e-5f)
        throw std::invalid_argument("rebase requires an orthogonal parent frame");
    const glm::quat parentRotation = glm::quat_cast(glm::mat3(parentWorld) / scale.x);
    auto& local = root.transform();
    const glm::vec3 worldPosition = glm::vec3(parentWorld * glm::vec4(local.position, 1.f));
    local.position = glm::vec3(glm::inverse(parentWorld) * glm::vec4(rotation * worldPosition + translation, 1.f));
    local.rotation = glm::inverse(parentRotation) * rotation * parentRotation * local.rotation;
    root.updateTransforms(parentWorld, true);
#ifndef SAIDA_NO_PHYSICS
    root.traverse([&](Node& node, const glm::mat4&) {
        if (auto* body = node.asCollisionObject()) body->rebasePhysics(translation, rotation);
        if (auto* joint = node.asJointNode()) joint->detachJointFromPhysics();
    });
#endif
}

void Scene::rebaseOrigin(const glm::vec3& translation, const glm::quat& rotation) {
    for (const auto& child : children()) rebaseSubtree(*child, translation, rotation);
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
