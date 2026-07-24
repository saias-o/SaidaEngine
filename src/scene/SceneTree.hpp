#pragma once

#include "scene/Node.hpp"
#include "scene/SceneTimerQueue.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace saida {

class Scene;
class ResourceManager;
class Behaviour;

class SceneTree {
public:
    explicit SceneTree(ResourceManager& resources);
    ~SceneTree();
    SceneTree(const SceneTree&) = delete;
    SceneTree& operator=(const SceneTree&) = delete;

    // Move rather than copy the live scene to avoid duplicating live resources.
    Scene* mountWorld(std::unique_ptr<Scene> startScene);
    void unmountWorld();
    bool mounted() const { return world_ != nullptr; }
    Scene& world() { return *world_; }
    Scene& currentScene();
    ResourceManager& resources() { return resources_; }

    void setProjectRoot(const std::string& root) { projectRoot_ = root; }
    std::string resolveProjectPath(const std::string& path) const;

    // Cache serialized scenes so repeated instantiation avoids disk I/O.
    Node* instantiate(const std::string& scenePath, Node* parent = nullptr);

    void changeScene(const std::string& scenePath);
    void reloadScene();
    void requestFree(Node* node);
    void applyDeferred();
    bool quitRequested() const { return quitRequested_; }

    void setPaused(bool paused);
    bool paused() const;
    void quit() { quitRequested_ = true; }

    TimerId after(Node* owner, float seconds, std::function<void()> fn);
    TimerId every(Node* owner, float interval, std::function<void()> fn);
    TimerId tween(Node* owner, float duration, Easing easing,
                  std::function<void(float)> fn);
    TimerId after(Behaviour* owner, float seconds, std::function<void()> fn);
    TimerId every(Behaviour* owner, float interval, std::function<void()> fn);
    TimerId tween(Behaviour* owner, float duration, Easing easing,
                  std::function<void(float)> fn);
    void cancelTimer(TimerId id);
    void cancelTimersOwnedBy(Node* owner);
    void cancelTimersOwnedBy(Behaviour* owner);
    void tickTimers(float dt);

    template <typename T>
    void registerAutoload(const std::string& name) {
        setAutoloadDef(name, [name] {
            auto node = std::make_unique<Node>(name);
            node->addBehaviour<T>();
            return node;
        });
    }
    bool registerAutoloadType(const std::string& name, const std::string& behaviourType);
    bool registerAutoloadScene(const std::string& name, const std::string& scenePath);
    // JS autoload: a Node carrying a ScriptBehaviour bound to this script (.js/.mjs).
    void registerAutoloadScript(const std::string& name, const std::string& scriptPath);

    template <typename T>
    T* autoload() const {
        // GCC 12 requires `template` here for a dependent structured binding.
        for (const auto& [name, node] : autoloadNodes_)
            if (T* b = node->template getBehaviour<T>()) return b;
        return nullptr;
    }
    Node* autoloadNode(const std::string& name) const;
    Node* nodeById(NodeId id) const;

    const std::vector<Node*>& group(const std::string& name);
    Node* firstInGroup(const std::string& name);

    // Character bodies (CharacterVirtual: player, NPCs, cars) are NOT visible to
    // PhysicsWorld::raycast, so gameplay (weapons, AoE, aggro, pickups, run-overs)
    // would otherwise re-implement the same manual loops. These node-level queries
    // close that gap: each node is approximated by a sphere at its world origin.
    // Pass a non-empty `group` to restrict candidates (cheap and usually what you
    // want); empty scans the whole tree.

    struct NodeRayHit {
        Node* node = nullptr;   // null when nothing was hit
        float distance = 0.0f;  // along the ray
        glm::vec3 point{0.0f};
    };

    std::vector<Node*> overlapSphere(const glm::vec3& center, float radius,
                                     const std::string& group = "");

    NodeRayHit raycastNodes(const glm::vec3& origin, const glm::vec3& direction,
                            float maxDistance, float nodeRadius,
                            const std::string& group = "");

private:
    using AutoloadFactory = std::function<std::unique_ptr<Node>()>;
    void setAutoloadDef(const std::string& name, AutoloadFactory factory);
    std::string resolvePath(const std::string& path) const;
    void loadCurrentScene(std::unique_ptr<Node> sceneNode);
    void applyLevelSettings(Scene& level);
    void startAutoloads();
    void clearAutoloads();

    ResourceManager& resources_;
    std::unique_ptr<Scene> world_;
    Scene* currentScene_ = nullptr;
    std::string currentScenePath_;

    bool pendingChange_ = false;
    std::string pendingScenePath_;
    std::vector<Node*> pendingFree_;
    bool quitRequested_ = false;

    struct AutoloadDef {
        std::string name;
        AutoloadFactory factory;
    };
    std::vector<AutoloadDef> autoloadDefs_;
    std::unordered_map<std::string, Node*> autoloadNodes_;

    std::vector<Node*> groupBuffer_;

    std::string projectRoot_;
    std::unordered_map<std::string, std::string> sceneCache_;

    SceneTimerQueue timerQueue_;
    uint32_t lastUsageVersion_ = 0;  // GPU usage snapshot (mid-scene budget)
};

} // namespace saida
