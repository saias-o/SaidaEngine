#pragma once

#include "graphics/ResourceManager.hpp"
#include "scene/Node.hpp"

#include <unordered_map>
#include <utility>
#include <unordered_set>

namespace saida {

class MeshNode;
class LightNode;
class UICanvasNode;
class WebCanvasNode;
class WaterNode;
class ParticleSystemNode;
class CollisionObjectNode;
class JointNode;
class LODGroupBehaviour;

// Dense membership, with constant-time insertion/removal. Iteration order is
// unspecified; a refresh is the boundary between mutation and consumption.
template<class T> class SceneMembers {
public:
    const std::vector<T*>& values() const { return values_; }
    void add(T* value) {
        if (!value || slots_.find(value) != slots_.end()) return;
        slots_.emplace(value, values_.size());
        values_.push_back(value);
    }
    void remove(T* value) {
        auto it = slots_.find(value);
        if (it == slots_.end()) return;
        const size_t slot = it->second;
        T* last = values_.back();
        values_[slot] = last;
        slots_[last] = slot;
        values_.pop_back();
        slots_.erase(it);
    }
private:
    std::vector<T*> values_;
    std::unordered_map<T*, size_t> slots_;
};

// Incremental scene membership and resource ownership. Entries remember child
// identities so removals never dereference a destroyed node. Unchanged branches
// are skipped, and hidden/disabled nodes still retain their resources.
class SceneIndex {
public:
    void refresh(Node& root);
    const ResourceManager::AssetUsage& resources() const { return resources_; }
    uint64_t resourceVersion() const { return resourceVersion_; }
    size_t visitedNodes() const { return visited_; }
    uint64_t totalVisitedNodes() const { return totalVisited_; }
    bool contains(const Node* node) const { return entries_.find(const_cast<Node*>(node)) != entries_.end(); }

    SceneMembers<MeshNode> meshes;
    SceneMembers<LightNode> lights;
    SceneMembers<UICanvasNode> canvases;
    SceneMembers<WebCanvasNode> webCanvases;
    SceneMembers<WaterNode> water;
    SceneMembers<ParticleSystemNode> particles;
    SceneMembers<Behaviour> behaviours;
    SceneMembers<CollisionObjectNode> bodies;
    // Subsets called on every physics substep (`hasPhysicsStep`,
    // `hasPrePhysicsStep`). Usually empty or a handful.
    SceneMembers<Behaviour> physicsSteppers;
    SceneMembers<CollisionObjectNode> preSteppers;
    SceneMembers<JointNode> joints;
    SceneMembers<LODGroupBehaviour> lodGroups;
    size_t activeNodes = 0;

private:
    struct Entry {
        uint64_t subtree = 0, local = 0, resources = 0;
        bool active = false, visible = false;
        Node* parent = nullptr;
        std::vector<Node*> children;
        MeshNode* mesh = nullptr;
        LightNode* light = nullptr;
        UICanvasNode* canvas = nullptr;
        WebCanvasNode* webCanvas = nullptr;
        WaterNode* water = nullptr;
        ParticleSystemNode* particles = nullptr;
        CollisionObjectNode* body = nullptr;
        JointNode* joint = nullptr;
        std::vector<Behaviour*> behaviours;
        std::vector<LODGroupBehaviour*> lods;
        std::unique_ptr<ResourceManager::AssetUsage> usage;
    };
    void sync(Node& node, Node* parent, bool parentActive, bool parentVisible);
    void erase(Node* node);
    void unpublish(Entry& entry);
    void publish(Node& node, Entry& entry);
    void updateResources(Node& node, Entry& entry);
    void countResources(const ResourceManager::AssetUsage& usage, bool add);

    std::unordered_map<Node*, Entry> entries_;
    std::vector<std::pair<Node*, Node*>> removed_;
    // Both skies of a scene root: the one drawn and the one it fades into.
    std::unordered_map<Node*, std::pair<AssetID, AssetID>> skyboxes_;
    ResourceManager::AssetUsage resources_;
    std::unordered_map<const Mesh*, size_t> meshRefs_;
    std::unordered_map<const Material*, size_t> materialRefs_;
    std::unordered_map<AssetID, size_t> textureRefs_;
    std::unordered_map<const Rig*, size_t> rigRefs_;
    std::unordered_map<const AnimationClip*, size_t> animationRefs_;
    uint64_t resourceVersion_ = 0;
    size_t visited_ = 0;
    uint64_t totalVisited_ = 0;
};

} // namespace saida
