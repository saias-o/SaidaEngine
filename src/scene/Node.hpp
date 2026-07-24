#pragma once

#include "scene/Behaviour.hpp"
#include "scene/NodeId.hpp"
#include "scene/Transform.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace saida {

class Mesh;
class Material;
class LightNode;
class CollisionObjectNode;
class CharacterBodyNode;
class JointNode;
class ResourceManager;
class SceneTree;

class Node {
public:
    explicit Node(std::string name = "Node");
    virtual ~Node();
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    Node* addChild(std::unique_ptr<Node> child);

    Node* addChildAt(std::unique_ptr<Node> child, size_t index);

    bool removeChild(Node* child);

    std::unique_ptr<Node> detachChild(Node* child);


    template <typename T, typename... Args>
    T* createChild(Args&&... args) {
        static_assert(std::is_base_of_v<Node, T>, "T must derive from Node");
        auto child = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = child.get();
        addChild(std::move(child));
        return ptr;
    }

    template <typename T, typename... Args>
    T* addBehaviour(Args&&... args) {
        static_assert(std::is_base_of_v<Behaviour, T>, "T must derive from Behaviour");
        return static_cast<T*>(addBehaviour(std::make_unique<T>(std::forward<Args>(args)...)));
    }

    Behaviour* addBehaviour(std::unique_ptr<Behaviour> behaviour);
    Behaviour* addBehaviourAt(std::unique_ptr<Behaviour> behaviour, size_t index);

    void removeBehaviour(Behaviour* b);

    void clearChildren() { children_.clear(); }

    void updateTree(float dt);

    void updateTransforms(const glm::mat4& parentWorld, bool parentDirty);

    const glm::mat4& worldTransform() const { return worldTransform_; }

    SceneTree* tree() const;

    // Request safe removal of this node at end of frame (never destroy mid-update).
    void queueFree();

    virtual SceneTree* ownTree() const { return nullptr; }

    Transform& transform() { return transform_; }
    const Transform& transform() const { return transform_; }
    glm::mat4 localMatrix() const { return transform_.matrix(); }

    const std::string& name() const { return name_; }
    void setName(const std::string& name) { name_ = name; }
    NodeId id() const { return id_; }
    void assignSerializedId(NodeId id) { id_ = id != kNodeInvalid ? id : generateNodeId(); }
    void regenerateId() { id_ = generateNodeId(); }
    
    bool enabled() const { return enabled_; }
    void setEnabled(bool enabled);
    bool isActiveInHierarchy() const;

    Node* parent() const { return parent_; }
    const std::vector<std::unique_ptr<Node>>& children() const { return children_; }

    void traverse(const glm::mat4& parentWorld,
                  const std::function<void(Node&, const glm::mat4& world)>& visit);

    void traverse(const std::function<void(Node&, const glm::mat4& world)>& visit) {
        traverse(glm::mat4(1.0f), visit);
    }

    // Cheap virtual queries avoid RTTI during scene traversal.
    virtual Mesh* mesh() const { return nullptr; }
    virtual Material* material() const { return nullptr; }
    virtual LightNode* asLight() { return nullptr; }
    virtual const LightNode* asLightConst() const { return nullptr; }
    virtual CollisionObjectNode* asCollisionObject() { return nullptr; }
    virtual CharacterBodyNode* asCharacterBody() { return nullptr; }
    virtual JointNode* asJointNode() { return nullptr; }

    // Resolve a slash-separated node path from this node: "." = self, ".." =
    // parent, a name = child with that name; a leading '/' starts at the tree
    // root. Returns null when any segment fails to resolve.
    Node* findByPath(const std::string& path);

    virtual const char* typeName() const { return "Node"; }
    virtual void serialize(nlohmann::json& j, ResourceManager& resources) const;
    virtual void deserialize(const nlohmann::json& j, ResourceManager& resources);

    bool hasBehaviours() const { return !behaviours_.empty(); }
    int behaviourCount() const { return static_cast<int>(behaviours_.size()); }
    const std::vector<std::unique_ptr<Behaviour>>& behaviours() const { return behaviours_; }

    template<typename T>
    T* getBehaviour() const {
        for (auto& b : behaviours_) {
            if (auto* ptr = dynamic_cast<T*>(b.get())) {
                return ptr;
            }
        }
        return nullptr;
    }

    template<typename T>
    T* requireBehaviour() {
        if (T* b = getBehaviour<T>()) return b;
        return addBehaviour<T>();
    }

    template<typename T>
    T* findBehaviourInChildren() const {
        for (const auto& c : children_) {
            if (T* b = c->getBehaviour<T>()) return b;
            if (T* b = c->findBehaviourInChildren<T>()) return b;
        }
        return nullptr;
    }
    template<typename T>
    T* getChildNode() const {
        for (const auto& c : children_)
            if (T* n = dynamic_cast<T*>(c.get())) return n;
        return nullptr;
    }

    // Groups provide lookup without coupling gameplay to node names.
    void addToGroup(const std::string& group);
    void removeFromGroup(const std::string& group);
    bool isInGroup(const std::string& group) const;
    const std::vector<std::string>& groups() const { return groups_; }

    const std::string& importedFromPath() const { return importedFromPath_; }
    void setImportedFromPath(const std::string& path) { importedFromPath_ = path; }

protected:
    NodeId id_ = kNodeInvalid;
    std::string name_;
    std::string importedFromPath_;
    bool enabled_ = true;
    Transform transform_;
    Node* parent_ = nullptr;
    std::vector<std::unique_ptr<Node>> children_;
    std::vector<std::unique_ptr<Behaviour>> behaviours_;
    std::vector<std::string> groups_;

    glm::mat4 worldTransform_{1.0f};
    glm::mat4 lastLocalMatrix_{0.0f};

public:
    static uint32_t g_hierarchyVersion;
    static uint32_t g_transformVersion;
};

} // namespace saida
