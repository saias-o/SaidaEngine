#include "scene/Node.hpp"
#include "scene/Behaviour.hpp"
#include "scene/BehaviourRegistry.hpp"
#include "scene/SceneTree.hpp"
#include "scene/SerializationHelpers.hpp"
#include "core/Log.hpp"

#ifndef SAIDA_NO_AUDIO
#include "audio/AudioManager.hpp"
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace saida {

uint32_t Node::g_hierarchyVersion = 1;
uint32_t Node::g_transformVersion = 1;

Node::Node(std::string name) : id_(generateNodeId()), name_(std::move(name)) {}

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
    g_hierarchyVersion++;
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
    g_hierarchyVersion++;
    return ptr;
}

Behaviour* Node::addBehaviour(std::unique_ptr<Behaviour> behaviour) {
    behaviour->node_ = this;
    Behaviour* ptr = behaviour.get();
    behaviours_.push_back(std::move(behaviour));
    g_hierarchyVersion++;
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
    g_hierarchyVersion++;
    return ptr;
}

void Node::removeBehaviour(Behaviour* b) {
    if (!b) return;
    for (auto it = behaviours_.begin(); it != behaviours_.end(); ++it) {
        if (it->get() == b) {
            if (b->ready_) b->onDestroy();
            b->cancelTimers();
            behaviours_.erase(it);
            g_hierarchyVersion++;
            return;
        }
    }
}

bool Node::removeChild(Node* child) {
    if (!child) return false;
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() == child) {
            children_.erase(it);
            g_hierarchyVersion++;
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
            g_hierarchyVersion++;
            return detached;
        }
    }
    return nullptr;
}

void Node::setEnabled(bool enabled) {
    if (enabled_ != enabled) {
        enabled_ = enabled;
        g_hierarchyVersion++;
    }
}

bool Node::isActiveInHierarchy() const {
    if (!enabled_) return false;
    if (parent_) return parent_->isActiveInHierarchy();
    return true;
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

void Node::updateTransforms(const glm::mat4& parentWorld, bool parentDirty) {
    glm::mat4 currentLocal = localMatrix();
    bool dirty = parentDirty || (currentLocal != lastLocalMatrix_);
    
    if (dirty) {
        worldTransform_ = parentWorld * currentLocal;
        lastLocalMatrix_ = currentLocal;
        ++g_transformVersion;
    }
    
    for (auto& child : children_) {
        child->updateTransforms(worldTransform_, dirty);
    }
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
