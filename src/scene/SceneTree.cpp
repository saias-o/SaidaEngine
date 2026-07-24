#include "scene/SceneTree.hpp"

#include "scene/Scene.hpp"
#include "nodes/MeshNode.hpp"
#include "nodes/UIImageNode.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/BehaviourRegistry.hpp"
#include "scripting/ScriptBehaviour.hpp"
#include "graphics/Material.hpp"
#include "graphics/ResourceManager.hpp"
#include "core/Time.hpp"
#include "core/Log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace saida {

SceneTree::SceneTree(ResourceManager& resources) : resources_(resources) {}
SceneTree::~SceneTree() { unmountWorld(); }

Scene* SceneTree::mountWorld(std::unique_ptr<Scene> startScene) {
    world_ = std::make_unique<Scene>();
    world_->setName("World");
    world_->setTree(this);

    // Autoloads must become ready before level behaviours: gameplay scripts may
    // resolve and call them from their first onReady callback.
    startAutoloads();
    if (startScene) loadCurrentScene(std::move(startScene));
    return world_.get();
}

void SceneTree::unmountWorld() {
    if (!world_) return;
    clearAutoloads();
    currentScene_ = nullptr;
    currentScenePath_.clear();
    pendingFree_.clear();
    pendingChange_ = false;
    quitRequested_ = false;
    world_.reset();  // ~Scene clears children (and their physics bodies) while alive
    timerQueue_.clear();
}

Scene& SceneTree::currentScene() {
    return currentScene_ ? *currentScene_ : *world_;
}

void SceneTree::loadCurrentScene(std::unique_ptr<Node> sceneNode) {
    if (!world_ || !sceneNode) return;
    if (currentScene_) world_->removeChild(currentScene_);

    Node* raw = world_->addChild(std::move(sceneNode));
    currentScene_ = dynamic_cast<Scene*>(raw);
    applyLevelSettings(currentScene_ ? *currentScene_ : *world_);

    // Wire data-driven signal→slot connections now that the sub-scene is live
    // (signals only fire during Play, which is exactly when this runs).
    if (currentScene_) currentScene_->applyConnections();
}

void SceneTree::applyLevelSettings(Scene& level) {
    // A level marked "change rendering at load" imposes its environment on the
    // World; otherwise the World keeps the settings it already has.
    if (&level != world_.get() && level.settings().changeRenderingAtLoad)
        world_->settings() = level.settings();
}

std::string SceneTree::resolvePath(const std::string& path) const {
    std::filesystem::path p(path);
    if (p.is_absolute() || projectRoot_.empty()) return path;
    return projectRoot_ + "/" + path;
}

std::string SceneTree::resolveProjectPath(const std::string& path) const {
    return resolvePath(path);
}

Node* SceneTree::instantiate(const std::string& scenePath, Node* parent) {
    // Cache the inner scene-node JSON (type forced to Scene) so repeated spawns
    // re-deserialize from memory instead of re-reading the file.
    auto it = sceneCache_.find(scenePath);
    if (it == sceneCache_.end()) {
        std::ifstream file(resolvePath(scenePath));
        if (!file.is_open()) {
            Log::warn("instantiate: cannot open '", scenePath, "'");
            return nullptr;
        }
        try {
            nlohmann::json doc = nlohmann::json::parse(file);
            nlohmann::json node = doc.at("scene");
            node["type"] = "Scene";
            it = sceneCache_.emplace(scenePath, node.dump()).first;
        } catch (const std::exception& e) {
            Log::warn("instantiate: ", e.what());
            return nullptr;
        }
    }

    auto node = SceneSerializer::nodeFromJson(it->second, resources_);
    if (!node) return nullptr;

    Node* target = parent ? parent : (currentScene_ ? currentScene_ : world_.get());
    if (!target) return nullptr;
    return target->addChild(std::move(node));
}

void SceneTree::changeScene(const std::string& scenePath) {
    pendingScenePath_ = scenePath;
    pendingChange_ = true;
}

void SceneTree::reloadScene() {
    if (!currentScenePath_.empty()) changeScene(currentScenePath_);
}

void SceneTree::requestFree(Node* node) {
    if (node) pendingFree_.push_back(node);
}

namespace {
// Marks everything this node (and its descendants) references in the
// ResourceManager: meshes + materials (and their textures) of MeshNode and
// their LODs, textures of UIImageNode, skybox of traversed Scenes.
void collectAssetUsage(Node& node, ResourceManager::AssetUsage& usage) {
    if (auto* meshNode = dynamic_cast<MeshNode*>(&node)) {
        auto markMaterial = [&usage](Material* material) {
            if (!material) return;
            usage.materials.insert(material);
            const MaterialDesc& d = material->desc();
            for (AssetID id : {d.albedoId, d.normalId, d.metallicRoughnessId, d.emissiveId})
                if (id != kAssetInvalid) usage.textures.insert(id);
        };
        if (meshNode->mesh()) usage.meshes.insert(meshNode->mesh());
        markMaterial(meshNode->material());
        for (const MeshLodLevel& lvl : meshNode->lods()) {
            if (lvl.mesh) usage.meshes.insert(lvl.mesh);
            markMaterial(lvl.material);
        }
    } else if (std::strcmp(node.typeName(), "UIImageNode") == 0) {
        // static_cast via typeName: the web player doesn't compile in UI nodes
        // (they degrade to a generic Node), and a dynamic_cast would require
        // their typeinfo at link time.
        auto* image = static_cast<UIImageNode*>(&node);
        if (image->texture() != kAssetInvalid) usage.textures.insert(image->texture());
    }
    if (auto* scene = dynamic_cast<Scene*>(&node)) {
        if (scene->settings().skyboxTexture != kAssetInvalid)
            usage.textures.insert(scene->settings().skyboxTexture);
    }
    // Animation: Animators hold raw pointers to a rig and clips — anything a
    // live Animator references must survive the trim.
    for (const auto& behaviour : node.behaviours()) {
        if (auto* animator = dynamic_cast<Animator*>(behaviour.get())) {
            if (animator->rig()) usage.rigs.insert(animator->rig());
            for (const auto& [name, clip] : animator->clips())
                if (clip) usage.animations.insert(clip);
        }
    }
    for (const auto& child : node.children()) collectAssetUsage(*child, usage);
}
} // namespace

void SceneTree::applyDeferred() {
    if (!world_) return;

    // 1) Honour queued frees (safe now that update is finished).
    for (Node* n : pendingFree_) {
        if (n && n->parent()) n->parent()->removeChild(n);
    }
    pendingFree_.clear();

    // 2) Swap the gameplay sub-scene if requested (World + autoloads untouched).
    bool sceneChanged = false;
    if (pendingChange_) {
        pendingChange_ = false;
        if (auto sub = SceneSerializer::loadNodeFromSceneFile(resolvePath(pendingScenePath_), resources_)) {
            currentScenePath_ = pendingScenePath_;
            loadCurrentScene(std::move(sub));
            sceneChanged = true;
        } else {
            Log::warn("changeScene: failed to load '", pendingScenePath_, "'");
        }
    }

    // 3) After a scene change, anything the World (autoloads + new
    // sub-scene) no longer references is evicted from the ResourceManager —
    // GPU memory stays bounded across N hub<->arena cycles. The old
    // sub-scene is already destroyed, so the walk sees exactly what must survive.
    if (sceneChanged) {
        ResourceManager::AssetUsage usage;
        collectAssetUsage(*world_, usage);
        resources_.trimUnused(usage);
    }

    // 3b) Mid-scene GPU budget: the ResourceManager LRU-evicts whatever the
    // live scene no longer references. Its reference snapshot is refreshed
    // on every hierarchy change (spawn, queueFree, scene) — the walk costs
    // nothing on a stable hierarchy.
    if (lastUsageVersion_ != Node::g_hierarchyVersion) {
        lastUsageVersion_ = Node::g_hierarchyVersion;
        ResourceManager::AssetUsage usage;
        collectAssetUsage(*world_, usage);
        resources_.setLiveUsage(std::move(usage));
    }

    // 4) The World's flattened caches (meshes/lights) still point to the
    // nodes destroyed above; this frame's render would otherwise consume them.
    world_->refreshHierarchy();
}

void SceneTree::setPaused(bool paused) { Time::setScale(paused ? 0.0f : 1.0f); }
bool SceneTree::paused() const { return Time::scale() == 0.0f; }


TimerId SceneTree::after(Node* owner, float seconds, std::function<void()> fn) {
    return timerQueue_.after(owner, seconds, std::move(fn));
}
TimerId SceneTree::every(Node* owner, float interval, std::function<void()> fn) {
    return timerQueue_.every(owner, interval, std::move(fn));
}
TimerId SceneTree::tween(Node* owner, float duration, Easing easing,
                         std::function<void(float)> fn) {
    return timerQueue_.tween(owner, duration, easing, std::move(fn));
}

TimerId SceneTree::after(Behaviour* owner, float seconds, std::function<void()> fn) {
    return timerQueue_.after(owner, seconds, std::move(fn));
}
TimerId SceneTree::every(Behaviour* owner, float interval, std::function<void()> fn) {
    return timerQueue_.every(owner, interval, std::move(fn));
}
TimerId SceneTree::tween(Behaviour* owner, float duration, Easing easing,
                         std::function<void(float)> fn) {
    return timerQueue_.tween(owner, duration, easing, std::move(fn));
}

void SceneTree::cancelTimer(TimerId id) { timerQueue_.cancel(id); }

void SceneTree::cancelTimersOwnedBy(Node* owner) { timerQueue_.cancelOwnedBy(owner); }

void SceneTree::cancelTimersOwnedBy(Behaviour* owner) { timerQueue_.cancelOwnedBy(owner); }

void SceneTree::tickTimers(float dt) { timerQueue_.tick(dt); }


void SceneTree::setAutoloadDef(const std::string& name, AutoloadFactory factory) {
    for (auto& def : autoloadDefs_) {
        if (def.name == name) {  // dedup: a re-registration replaces the old one
            def.factory = std::move(factory);
            return;
        }
    }
    autoloadDefs_.push_back({name, std::move(factory)});
}

bool SceneTree::registerAutoloadType(const std::string& name, const std::string& behaviourType) {
    if (BehaviourRegistry::instance().factories().find(behaviourType) ==
        BehaviourRegistry::instance().factories().end()) {
        Log::error("autoload '", name, "': unsupported behaviour type '", behaviourType, "'");
        return false;
    }
    setAutoloadDef(name, [name, behaviourType] {
        auto node = std::make_unique<Node>(name);
        if (auto b = BehaviourRegistry::instance().create(behaviourType))
            node->addBehaviour(std::move(b));
        return node;
    });
    return true;
}

bool SceneTree::registerAutoloadScene(const std::string& name, const std::string& scenePath) {
    // Validate eagerly so a required singleton cannot disappear while the
    // player still reports a successful boot.
    if (!SceneSerializer::loadNodeFromSceneFile(scenePath, resources_)) {
        Log::error("autoload '", name, "': failed scene contract preflight for '", scenePath, "'");
        return false;
    }
    setAutoloadDef(name, [this, name, scenePath] {
        auto node = SceneSerializer::loadNodeFromSceneFile(scenePath, resources_);
        if (node) node->setName(name);
        else Log::error("autoload '", name, "': failed to load scene '", scenePath, "'");
        return node;
    });
    return true;
}

void SceneTree::registerAutoloadScript(const std::string& name,
                                       const std::string& scriptPath) {
    setAutoloadDef(name, [name, scriptPath] {
        auto node = std::make_unique<Node>(name);
        auto script = std::make_unique<ScriptBehaviour>();
        script->setScriptPath(scriptPath);
        node->addBehaviour(std::move(script));
        return node;
    });
}

void SceneTree::startAutoloads() {
    if (!world_) return;
    for (const auto& def : autoloadDefs_) {
        if (auto node = def.factory()) {
            Node* raw = world_->addChild(std::move(node));
            autoloadNodes_[def.name] = raw;
        }
    }
}

void SceneTree::clearAutoloads() {
    autoloadNodes_.clear();  // nodes are owned by (and destroyed with) the World
}

Node* SceneTree::autoloadNode(const std::string& name) const {
    auto it = autoloadNodes_.find(name);
    return it != autoloadNodes_.end() ? it->second : nullptr;
}


namespace {
Node* findNodeById(Node& node, NodeId id) {
    if (node.id() == id) return &node;
    for (const auto& child : node.children()) {
        if (Node* found = findNodeById(*child, id)) return found;
    }
    return nullptr;
}

void collectGroup(Node& n, const std::string& g, std::vector<Node*>& out) {
    if (n.isInGroup(g)) out.push_back(&n);
    for (const auto& c : n.children()) collectGroup(*c, g, out);
}
void collectAll(Node& n, std::vector<Node*>& out) {
    out.push_back(&n);
    for (const auto& c : n.children()) collectAll(*c, out);
}
Node* firstInGroupRec(Node& n, const std::string& g) {
    if (n.isInGroup(g)) return &n;
    for (const auto& c : n.children())
        if (Node* found = firstInGroupRec(*c, g)) return found;
    return nullptr;
}
} // namespace

Node* SceneTree::nodeById(NodeId id) const {
    return world_ && id != kNodeInvalid ? findNodeById(*world_, id) : nullptr;
}

const std::vector<Node*>& SceneTree::group(const std::string& name) {
    groupBuffer_.clear();
    if (world_) collectGroup(*world_, name, groupBuffer_);
    return groupBuffer_;
}

Node* SceneTree::firstInGroup(const std::string& name) {
    return world_ ? firstInGroupRec(*world_, name) : nullptr;
}


std::vector<Node*> SceneTree::overlapSphere(const glm::vec3& center, float radius,
                                            const std::string& group) {
    std::vector<Node*> candidates, result;
    if (!world_) return result;
    if (group.empty()) collectAll(*world_, candidates);
    else collectGroup(*world_, group, candidates);

    const float r2 = radius * radius;
    for (Node* n : candidates) {
        glm::vec3 d = glm::vec3(n->worldTransform()[3]) - center;
        if (glm::dot(d, d) <= r2) result.push_back(n);
    }
    return result;
}

SceneTree::NodeRayHit SceneTree::raycastNodes(const glm::vec3& origin,
                                              const glm::vec3& direction,
                                              float maxDistance, float nodeRadius,
                                              const std::string& group) {
    NodeRayHit best;
    if (!world_) return best;

    float len2 = glm::dot(direction, direction);
    if (len2 < 1e-12f) return best;
    glm::vec3 dir = direction / std::sqrt(len2);

    std::vector<Node*> candidates;
    if (group.empty()) collectAll(*world_, candidates);
    else collectGroup(*world_, group, candidates);

    float bestT = maxDistance;
    for (Node* n : candidates) {
        glm::vec3 oc = origin - glm::vec3(n->worldTransform()[3]);
        float b = glm::dot(dir, oc);
        float c = glm::dot(oc, oc) - nodeRadius * nodeRadius;
        float disc = b * b - c;
        if (disc < 0.0f) continue;
        float s = std::sqrt(disc);
        float t = -b - s;
        if (t < 0.0f) t = -b + s;        // origin inside the sphere → use far root
        if (t < 0.0f || t > bestT) continue;
        bestT = t;
        best.node = n;
        best.distance = t;
        best.point = origin + dir * t;
    }
    return best;
}

} // namespace saida
