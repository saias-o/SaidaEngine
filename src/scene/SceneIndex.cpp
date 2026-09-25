#include "scene/SceneIndex.hpp"

#include "scene/Scene.hpp"
#include "scene/animation/Animator.hpp"
#include "behaviours/LODGroupBehaviour.hpp"
#include "nodes/MeshNode.hpp"
#ifndef SAIDA_NO_PHYSICS
#include "physics/CollisionObjectNode.hpp"
#endif
#include "nodes/LightNode.hpp"
#include "nodes/WaterNode.hpp"
#include "nodes/UICanvasNode.hpp"
#include "nodes/UIImageNode.hpp"
#include "nodes/ParticleSystemNode.hpp"
#include "graphics/Material.hpp"
#ifndef SAIDA_RHI_WEBGPU
#include "nodes/WebCanvasNode.hpp"
#endif

#include <cstring>

namespace saida {
namespace {
template<class T> void count(const std::unordered_set<T>& values, bool add,
                            std::unordered_map<T, size_t>& refs, std::unordered_set<T>& live) {
    for (T value : values) {
        if (add) {
            if (++refs[value] == 1) live.insert(value);
        } else if (auto it = refs.find(value); it != refs.end() && --it->second == 0) {
            live.erase(value);
            refs.erase(it);
        }
    }
}
}

void SceneIndex::countResources(const ResourceManager::AssetUsage& usage, bool add) {
    count(usage.meshes, add, meshRefs_, resources_.meshes);
    count(usage.materials, add, materialRefs_, resources_.materials);
    count(usage.textures, add, textureRefs_, resources_.textures);
    count(usage.rigs, add, rigRefs_, resources_.rigs);
    count(usage.animations, add, animationRefs_, resources_.animations);
    ++resourceVersion_;
}

void SceneIndex::updateResources(Node& node, Entry& entry) {
    if (entry.usage) countResources(*entry.usage, false);
    ResourceManager::AssetUsage usage;
    auto material = [&](Material* m) {
        if (!m) return;
        usage.materials.insert(m);
        const auto& d = m->desc();
        for (AssetID id : {d.albedoId, d.normalId, d.metallicRoughnessId, d.emissiveId})
            if (id != kAssetInvalid) usage.textures.insert(id);
    };
    if (auto* mesh = dynamic_cast<const MeshNode*>(&node)) {
        if (mesh->mesh()) usage.meshes.insert(mesh->mesh());
        material(mesh->material());
        for (const auto& level : mesh->lods()) {
            if (level.mesh) usage.meshes.insert(level.mesh);
            material(level.material);
        }
    } else if (std::strcmp(node.typeName(), "UIImageNode") == 0) {
        const AssetID id = static_cast<UIImageNode&>(node).texture();
        if (id != kAssetInvalid) usage.textures.insert(id);
    }
    if (auto* scene = dynamic_cast<Scene*>(&node)) {
        const auto& settings = scene->settings();
        skyboxes_[&node] = {settings.skyboxTexture, settings.skyboxBlendTexture};
        for (AssetID id : {settings.skyboxTexture, settings.skyboxBlendTexture})
            if (id != kAssetInvalid) usage.textures.insert(id);
    }
    for (const auto& b : node.behaviours()) {
        if (auto* animator = dynamic_cast<Animator*>(b.get())) {
            if (animator->rig()) usage.rigs.insert(animator->rig());
            for (const auto& [name, clip] : animator->clips())
                if (clip) usage.animations.insert(clip);
        }
    }
    const bool empty = usage.meshes.empty() && usage.materials.empty() && usage.textures.empty()
                    && usage.rigs.empty() && usage.animations.empty();
    entry.usage = empty ? nullptr : std::make_unique<ResourceManager::AssetUsage>(std::move(usage));
    if (entry.usage) countResources(*entry.usage, true);
    entry.resources = node.resourceRevision();
}

void SceneIndex::unpublish(Entry& e) {
    meshes.remove(e.mesh); lights.remove(e.light); canvases.remove(e.canvas);
    webCanvases.remove(e.webCanvas); water.remove(e.water); particles.remove(e.particles);
    bodies.remove(e.body); joints.remove(e.joint); preSteppers.remove(e.body);
    for (auto* b : e.behaviours) { behaviours.remove(b); physicsSteppers.remove(b); }
    e.behaviours.clear();
    for (auto* lod : e.lods) lodGroups.remove(lod);
    e.lods.clear();
    if (e.active) --activeNodes;
}

void SceneIndex::publish(Node& node, Entry& e) {
    e.mesh = dynamic_cast<MeshNode*>(&node);
    e.light = node.asLight();
    e.canvas = dynamic_cast<UICanvasNode*>(&node);
    e.water = dynamic_cast<WaterNode*>(&node);
    e.particles = dynamic_cast<ParticleSystemNode*>(&node);
#ifndef SAIDA_RHI_WEBGPU
    e.webCanvas = dynamic_cast<WebCanvasNode*>(&node);
#endif
    e.body = node.asCollisionObject();
    e.joint = node.asJointNode();
    if (!e.active) {
#ifndef SAIDA_NO_PHYSICS
        // A disabled body leaves the simulation, not only this index: left in
        // Jolt it would stay where it was disabled, unseen, never synced, and
        // still solid. Enabled again, its next sync builds it afresh.
        if (e.body) e.body->detachFromPhysics();
#endif
        return;
    }
    ++activeNodes;
    bodies.add(e.body); joints.add(e.joint);
    if (e.body && e.body->hasPrePhysicsStep()) preSteppers.add(e.body);
    for (const auto& b : node.behaviours()) {
        e.behaviours.push_back(b.get());
        behaviours.add(b.get());
        if (b->hasPhysicsStep()) physicsSteppers.add(b.get());
        if (auto* lod = dynamic_cast<LODGroupBehaviour*>(b.get())) {
            e.lods.push_back(lod);
            lodGroups.add(lod);
        }
    }
    if (!e.visible) return;
    if (e.mesh && e.mesh->meshEnabled()) meshes.add(e.mesh);
    lights.add(e.light); canvases.add(e.canvas); webCanvases.add(e.webCanvas);
    water.add(e.water); particles.add(e.particles);
}

void SceneIndex::erase(Node* node) {
    auto it = entries_.find(node);
    if (it == entries_.end()) return;
    Entry& e = it->second;
    for (Node* child : e.children) {
        auto found = entries_.find(child);
        if (found != entries_.end() && found->second.parent == node) erase(child);
    }
    unpublish(e);
    if (e.usage) countResources(*e.usage, false);
    skyboxes_.erase(node);
    entries_.erase(it);
}

void SceneIndex::sync(Node& node, Node* parent, bool parentActive, bool parentVisible) {
    Entry& e = entries_[&node];
    e.parent = parent;
    const bool active = parentActive && node.enabled();
    const bool visible = parentVisible && node.visible();
    if (e.subtree == node.subtreeRevision() && e.active == active && e.visible == visible) return;
    ++visited_;
    ++totalVisited_;
    const bool localChanged = e.local != node.localRevision();
    if (localChanged) {
        std::unordered_set<Node*> current;
        for (const auto& child : node.children()) current.insert(child.get());
        for (Node* child : e.children) if (current.find(child) == current.end()) removed_.emplace_back(child, &node);
        e.children.clear();
        for (const auto& child : node.children()) e.children.push_back(child.get());
    }
    if (localChanged || e.active != active || e.visible != visible) {
        unpublish(e);
        e.active = active;
        e.visible = visible;
        publish(node, e);
    }
    if (e.resources != node.resourceRevision()) updateResources(node, e);
    for (const auto& child : node.children()) sync(*child, &node, active, visible);
    e.local = node.localRevision();
    e.subtree = node.subtreeRevision();
}

void SceneIndex::refresh(Node& root) {
    visited_ = 0;
    removed_.clear();
    sync(root, nullptr, true, true);
    // A detached subtree can have been reparented earlier in this same refresh.
    for (auto [node, formerParent] : removed_) {
        auto it = entries_.find(node);
        if (it != entries_.end() && it->second.parent == formerParent) erase(node);
    }
    // Settings are an existing mutable struct; inspect only scene roots to
    // detect a changed skybox, not every node or every resource in the world.
    for (auto& [node, previous] : skyboxes_) {
        const auto& settings = static_cast<Scene*>(node)->settings();
        const std::pair<AssetID, AssetID> current{settings.skyboxTexture, settings.skyboxBlendTexture};
        if (current != previous) {
            updateResources(*node, entries_.at(node));
            previous = current;
        }
    }
}

} // namespace saida
