#include "scene/Node.hpp"
#include "scene/Behaviour.hpp"
#include "scene/BehaviourRegistry.hpp"
#include "scene/SceneTree.hpp"
#include "scene/SerializationHelpers.hpp"
#include "core/Log.hpp"
#include "core/Profiler.hpp"

#ifndef SAIDA_NO_AUDIO
#include "audio/AudioManager.hpp"
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace saida {

uint32_t Node::g_hierarchyVersion = 1;
uint32_t Node::g_transformVersion = 1;

struct Node::TransformTraversal {
    static_assert(std::is_trivially_copyable_v<Transform>);
    struct Entry {
        Node* node;
        size_t parent;
        Transform observed;
        bool fresh = true;
        bool dirty = false;
        bool nonFinite = false;
    };
    struct Branch {
        size_t end;
        uint64_t topology;
    };
    std::vector<Entry> entries;
    std::vector<Branch> branches;
    std::vector<Entry> staging;
    std::vector<Branch> stagingBranches;
    uint64_t topology = 0;

    void refresh(Node& root) {
        staging.clear();stagingBranches.clear();
        staging.reserve(entries.size());stagingBranches.reserve(branches.size());
        constexpr size_t missing = size_t(-1);
        auto append = [&](auto&& self, Node* node, size_t parent, size_t old) -> void {
            const size_t slot = staging.size();
            const bool matched = old != missing && old < entries.size() && entries[old].node == node;
            if (matched && branches[old].topology == node->topologyRevision_) {
                // Keep unobserved retained-reference writes detectable. Reuse
                // snapshots, never assume an unchanged topology is immobile.
                for (size_t j = old; j < branches[old].end; ++j) {
                    auto entry = entries[j];
                    entry.parent = j == old ? parent : entry.parent - old + slot;
                    staging.push_back(entry);
                    stagingBranches.push_back({branches[j].end - old + slot, branches[j].topology});
                }
                staging[slot].fresh = true; // The branch may have a new parent.
                return;
            }
            staging.push_back({node, parent, node->transform_});
            stagingBranches.push_back({0, node->topologyRevision_});
            size_t next = matched ? old + 1 : missing;
            const size_t oldEnd = matched ? branches[old].end : 0;
            std::unordered_map<Node*, size_t> siblings;
            for (const auto& child : node->children_) {
                size_t previous = missing;
                if (matched) {
                    if (next < oldEnd && entries[next].node == child.get()) previous = next;
                    else {
                        // A changed child list needs one lookup table, not a
                        // quadratic search when a large branch is reordered.
                        if (siblings.empty())
                            for (size_t j = old + 1; j < oldEnd; j = branches[j].end)
                                siblings.emplace(entries[j].node, j);
                        const auto found = siblings.find(child.get());
                        if (found != siblings.end()) previous = found->second;
                    }
                }
                self(self, child.get(), slot, previous);
                if (previous != missing) next = branches[previous].end;
            }
            stagingBranches[slot].end = staging.size();
        };
        append(append, &root, 0, entries.empty() ? missing : 0);
        entries.swap(staging);branches.swap(stagingBranches);
        topology = root.topologyRevision_;
    }
};

Node::Node(std::string name) : id_(generateNodeId()), name_(std::move(name)) {
    markResourcesChanged();
}

void Node::markChanged() {
    static uint64_t revision = 0;
    localRevision_ = ++revision;
    for (Node* n = this; n; n = n->parent_) n->subtreeRevision_ = revision;
    ++g_hierarchyVersion; // legacy public counter; internal caches use subtree revisions
}

void Node::markResourcesChanged() {
    markChanged();
    resourceRevision_ = localRevision_;
}

void Node::markTopologyChanged() {
    static uint64_t revision = 1;
    ++revision;
    for (Node* n = this; n; n = n->parent_) n->topologyRevision_ = revision;
}

void Node::clearChildren() {
    if (children_.empty()) return;
    children_.clear();
    markTopologyChanged();
    markChanged();
}

Node::~Node() {
    // Notify behaviours that ran, then cancel this node's timers — while the node
    // (and its parent chain, so tree()) is still valid.
    for (auto& b : behaviours_) {
        if (b->ready_) b->onDestroy();
        b->cancelTimers();
    }
#ifdef SAIDA_SCENE_TREE_TIMERS
    if (SceneTree* t = tree()) t->cancelTimersOwnedBy(this);
#endif
#ifndef SAIDA_NO_AUDIO
    AudioManager::get().stopAllOnNode(this);
#endif
}

Node* Node::addChild(std::unique_ptr<Node> child) {
    child->parent_ = this;
    Node* ptr = child.get();
    children_.push_back(std::move(child));
    markTopologyChanged();
    markChanged();
    return ptr;
}

Node* Node::addChildAt(std::unique_ptr<Node> child, size_t index) {
    child->parent_ = this;
    Node* ptr = child.get();
    if (index >= children_.size()) {
        children_.push_back(std::move(child));
    } else {
        children_.insert(children_.begin() + index, std::move(child));
    }
    markTopologyChanged();
    markChanged();
    return ptr;
}

Behaviour* Node::addBehaviour(std::unique_ptr<Behaviour> behaviour) {
    behaviour->node_ = this;
    Behaviour* ptr = behaviour.get();
    behaviours_.push_back(std::move(behaviour));
    markResourcesChanged();
    return ptr;
}

Behaviour* Node::addBehaviourAt(std::unique_ptr<Behaviour> behaviour, size_t index) {
    behaviour->node_ = this;
    Behaviour* ptr = behaviour.get();
    if (index >= behaviours_.size()) {
        behaviours_.push_back(std::move(behaviour));
    } else {
        behaviours_.insert(behaviours_.begin() + index, std::move(behaviour));
    }
    markResourcesChanged();
    return ptr;
}

void Node::removeBehaviour(Behaviour* b) {
    if (!b) return;
    for (auto it = behaviours_.begin(); it != behaviours_.end(); ++it) {
        if (it->get() == b) {
            if (b->ready_) b->onDestroy();
            b->cancelTimers();
            behaviours_.erase(it);
            markResourcesChanged();
            return;
        }
    }
}

bool Node::removeChild(Node* child) {
    if (!child) return false;
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() == child) {
            children_.erase(it);
            markTopologyChanged();
            markChanged();
            return true;
        }
    }
    return false;
}

std::unique_ptr<Node> Node::detachChild(Node* child) {
    if (!child) return nullptr;
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() == child) {
            std::unique_ptr<Node> detached = std::move(*it);
            children_.erase(it);
            detached->parent_ = nullptr;
            markTopologyChanged();
            markChanged();
            return detached;
        }
    }
    return nullptr;
}

void Node::setEnabled(bool enabled) {
    if (enabled_ != enabled) {
        enabled_ = enabled;
        markChanged();
    }
}

bool Node::isActiveInHierarchy() const {
    if (!enabled_) return false;
    if (parent_) return parent_->isActiveInHierarchy();
    return true;
}

void Node::setVisible(bool visible) {
    if (visible_ == visible) return;
    visible_ = visible;
    markChanged();
}

bool Node::isVisibleInHierarchy() const {
    return visible_ && (!parent_ || parent_->isVisibleInHierarchy());
}

SceneTree* Node::tree() const {
    const Node* root = this;
    while (root->parent_) root = root->parent_;
    return root->ownTree();
}

void Node::queueFree() {
    if (SceneTree* t = tree()) t->requestFree(this);
}

void Node::addToGroup(const std::string& group) {
    if (std::find(groups_.begin(), groups_.end(), group) == groups_.end())
        groups_.push_back(group);
}

void Node::removeFromGroup(const std::string& group) {
    groups_.erase(std::remove(groups_.begin(), groups_.end(), group), groups_.end());
}

bool Node::isInGroup(const std::string& group) const {
    return std::find(groups_.begin(), groups_.end(), group) != groups_.end();
}

void Node::updateTree(float dt) {
    for (auto& behaviour : behaviours_) {
        if (!behaviour->enabled()) continue;
        if (!behaviour->ready_) {
            behaviour->onReady();
            behaviour->ready_ = true;
        }
        behaviour->onUpdate(dt);
    }
    for (auto& child : children_)
        child->updateTree(dt);
}

glm::mat4 Node::localMatrix() const {
    if (!localMatrixCached_ || transform_.position != cachedLocalTransform_.position ||
        transform_.rotation != cachedLocalTransform_.rotation || transform_.scale != cachedLocalTransform_.scale) {
        cachedLocalTransform_ = transform_;
        cachedLocalMatrix_ = transform_.matrix();
        localMatrixCached_ = true;
    }
    return cachedLocalMatrix_;
}

void Node::updateTransforms(const glm::mat4& parentWorld, bool parentDirty) {
    if (!transformTraversal_) transformTraversal_ = std::make_unique<TransformTraversal>();
    auto& walk = *transformTraversal_;
    const bool rebuild = walk.topology != topologyRevision_;
    if (rebuild) {
        SAIDA_PROFILE_SCOPE("Transforms/Rebuild");
        walk.refresh(*this);
    }

    static uint64_t revision = 0;
    uint64_t batchRevision = 0;
    size_t recomputed = 0;
    for (size_t i = 0; i < walk.entries.size(); ++i) {
        auto& entry = walk.entries[i];
        Node& node = *entry.node;
#if defined(__GNUC__) || defined(__clang__)
        constexpr size_t prefetchDistance = 32;
        if (i + prefetchDistance < walk.entries.size())
            __builtin_prefetch(&walk.entries[i + prefetchDistance].node->transform_, 0, 1);
#endif
        const bool inherited = i == 0 ? parentDirty : walk.entries[entry.parent].dirty;
        const auto& local = node.transform_;
        // Retained Transform& writes bypass every setter. Compare even hidden
        // and disabled nodes, but touch matrices only on affected branches.
        const bool changed = entry.fresh || entry.nonFinite ||
            std::memcmp(&local, &entry.observed, sizeof(Transform)) != 0;
        const bool reparented = (entry.fresh || i == 0) && node.lastTransformParent_ != node.parent_;
        entry.dirty = false;
        if (changed || inherited || reparented) {
            entry.fresh = false;
            if (changed) {
                // Copy padding as well, if a platform aligns GLM components.
                // Matrix comparison still decides whether signed zero or a
                // different quaternion representation really changed the pose.
                std::memcpy(&entry.observed, &local, sizeof(Transform));
                auto finite3 = [](const glm::vec3& v) {
                    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
                };
                entry.nonFinite = !finite3(local.position) || !finite3(local.scale) ||
                    !std::isfinite(local.rotation.x) || !std::isfinite(local.rotation.y) ||
                    !std::isfinite(local.rotation.z) || !std::isfinite(local.rotation.w);
            }
            const glm::mat4 currentLocal = node.localMatrix();
            entry.dirty = inherited || reparented || currentLocal != node.lastLocalMatrix_;
            if (entry.dirty) {
                const auto& above = i == 0 ? parentWorld : walk.entries[entry.parent].node->worldTransform_;
                node.worldTransform_ = above * currentLocal;
                node.lastLocalMatrix_ = currentLocal;
                node.lastTransformParent_ = node.parent_;
                ++g_transformVersion;
                ++recomputed;
                if (!batchRevision) batchRevision = ++revision;
                for (Node* n = &node; n && n->subtreeTransformRevision_ != batchRevision; n = n->parent_)
                    n->subtreeTransformRevision_ = batchRevision;
            }
        }
    }
    SAIDA_PROFILE_COUNTER("Transforms/Compared", walk.entries.size());
    SAIDA_PROFILE_COUNTER("Transforms/Recomputed", recomputed);
    SAIDA_PROFILE_COUNTER("Transforms/Rebuilt", rebuild ? 1 : 0);
}

Node* Node::findByPath(const std::string& path) {
    if (path.empty()) return nullptr;

    Node* current = this;
    std::size_t i = 0;
    if (path.front() == '/') {
        while (current->parent()) current = current->parent();
        i = 1;
    }

    while (i < path.size()) {
        std::size_t end = path.find('/', i);
        if (end == std::string::npos) end = path.size();
        const std::string segment = path.substr(i, end - i);
        i = end + 1;

        if (segment.empty() || segment == ".") continue;
        if (segment == "..") {
            current = current->parent();
            if (!current) return nullptr;
            continue;
        }
        Node* next = nullptr;
        for (const auto& child : current->children()) {
            if (child->name() == segment) {
                next = child.get();
                break;
            }
        }
        if (!next) return nullptr;
        current = next;
    }
    return current;
}

void Node::traverse(const glm::mat4& parentWorld,
                    const std::function<void(Node&, const glm::mat4&)>& visit) {
    glm::mat4 world = parentWorld * localMatrix();
    visit(*this, world);
    for (auto& child : children_)
        child->traverse(world, visit);
}

void Node::serialize(nlohmann::json& j, ResourceManager& resources) const {
    j["type"] = typeName();
    j["id"] = id();
    j["name"] = name();
    j["enabled"] = enabled();
    if (!importedFromPath_.empty())
        j["importedFrom"] = importedFromPath_;
    // Groups are the sanctioned lookup mechanism (e.g. "player"), so they must
    // round-trip. Omitted when empty to keep existing scenes unchanged.
    if (!groups_.empty())
        j["groups"] = groups_;

    const Transform& t = transform();
    j["transform"] = {
        {"position", vec3ToJson(t.position)},
        {"rotation", nlohmann::json::array({t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w})},
        {"scale", vec3ToJson(t.scale)},
    };

    nlohmann::json behavioursJson = nlohmann::json::array();
    for (const auto& b : behaviours()) {
        if (const char* tn = b->typeName()) {
            nlohmann::json bj;
            bj["type"] = tn;
            bj["enabled"] = b->enabled();
            b->save(bj);
            behavioursJson.push_back(std::move(bj));
        }
    }
    j["behaviours"] = std::move(behavioursJson);

    nlohmann::json childrenJson = nlohmann::json::array();
    for (const auto& child : children()) {
        nlohmann::json cj;
        child->serialize(cj, resources);
        childrenJson.push_back(std::move(cj));
    }
    j["children"] = std::move(childrenJson);
}

void Node::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    if (j.contains("name")) setName(j["name"].get<std::string>());
    if (j.contains("enabled")) setEnabled(j["enabled"].get<bool>());
    if (j.contains("importedFrom")) importedFromPath_ = j["importedFrom"].get<std::string>();
    if (j.contains("groups") && j["groups"].is_array())
        for (const auto& g : j["groups"])
            if (g.is_string()) addToGroup(g.get<std::string>());

    if (j.contains("transform")) {
        auto jt = j["transform"];
        Transform& t = transform();
        t.position = jsonToVec3(jt["position"]);
        if (jt.contains("rotation")) {
            auto r = jt["rotation"];
            if (r.is_array() && r.size() == 4) {
                t.rotation = glm::quat(r[3].get<float>(), r[0].get<float>(),
                                       r[1].get<float>(), r[2].get<float>());
            }
        }
        t.scale = jsonToVec3(jt["scale"], glm::vec3(1.0f));
    }

    if (j.contains("behaviours") && j["behaviours"].is_array()) {
        for (const auto& bj : j["behaviours"]) {
            if (!bj.contains("type")) continue;
            std::string tn = bj["type"].get<std::string>();
            if (auto b = BehaviourRegistry::instance().create(tn)) {
                if (bj.contains("enabled")) {
                    b->setEnabled(bj["enabled"].get<bool>());
                }
                b->load(bj);
                addBehaviour(std::move(b));
            } else {
                Log::warn("Failed to deserialize behaviour of type: ", tn);
            }
        }
    }

    // Note: children deserialization is handled by SceneSerializer directly
    // because it needs to use NodeRegistry to instantiate polymorphic children.
}

} // namespace saida
