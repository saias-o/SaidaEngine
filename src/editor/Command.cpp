#include "editor/Command.hpp"

#include "editor/SceneDocument.hpp"
#include "graphics/ResourceManager.hpp"
#include "scene/BehaviourRegistry.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSerializer.hpp"
#include "scripting/ScriptBehaviour.hpp"

#include <nlohmann/json.hpp>
#include <glm/gtc/quaternion.hpp>

namespace saida {
namespace {

size_t childIndex(const Node& parent, const Node* child) {
    const auto& children = parent.children();
    for (size_t i = 0; i < children.size(); ++i)
        if (children[i].get() == child) return i;
    return children.size();
}

Transform transformFromMatrix(const glm::mat4& matrix) {
    Transform result;
    result.position = glm::vec3(matrix[3]);
    glm::vec3 x(matrix[0]);
    glm::vec3 y(matrix[1]);
    glm::vec3 z(matrix[2]);
    result.scale = {glm::length(x), glm::length(y), glm::length(z)};
    constexpr float epsilon = 1e-6f;
    if (result.scale.x > epsilon) x /= result.scale.x;
    if (result.scale.y > epsilon) y /= result.scale.y;
    if (result.scale.z > epsilon) z /= result.scale.z;
    result.rotation = glm::normalize(glm::quat_cast(glm::mat3(x, y, z)));
    return result;
}

} // namespace

AddNodeCommand::AddNodeCommand(NodeId parent, std::unique_ptr<Node> node)
    : parentId_(parent), nodeId_(node ? node->id() : kNodeInvalid), pending_(std::move(node)) {}

void AddNodeCommand::execute(SceneDocument& document) {
    Node* parent = document.find(parentId_);
    if (!parent || nodeId_ == kNodeInvalid) return;
    if (!pending_ && !snapshot_.empty() && document.resources())
        pending_ = SceneSerializer::nodeFromJson(
            snapshot_, *document.resources(), NodeIdPolicy::Preserve);
    if (pending_) parent->addChild(std::move(pending_));
    document.markDirty();
}

void AddNodeCommand::undo(SceneDocument& document) {
    Node* node = document.find(nodeId_);
    Node* parent = document.find(parentId_);
    if (!node || !parent || !document.resources()) return;
    snapshot_ = SceneSerializer::nodeToJson(*node, *document.resources());
    parent->removeChild(node);
    document.markDirty();
}

void DeleteNodeCommand::execute(SceneDocument& document) {
    Node* node = document.find(nodeId_);
    if (!node || !node->parent() || !document.resources()) return;
    if (parentId_ == kNodeInvalid) {
        parentId_ = node->parent()->id();
        index_ = childIndex(*node->parent(), node);
        snapshot_ = SceneSerializer::nodeToJson(*node, *document.resources());
    }
    node->parent()->removeChild(node);
    document.markDirty();
}

void DeleteNodeCommand::undo(SceneDocument& document) {
    Node* parent = document.find(parentId_);
    if (!parent || snapshot_.empty() || !document.resources()) return;
    if (auto restored = SceneSerializer::nodeFromJson(
            snapshot_, *document.resources(), NodeIdPolicy::Preserve))
        parent->addChildAt(std::move(restored), index_);
    document.markDirty();
}

void RenameNodeCommand::execute(SceneDocument& document) {
    if (Node* node = document.find(nodeId_)) node->setName(newName_);
    document.markDirty();
}

void RenameNodeCommand::undo(SceneDocument& document) {
    if (Node* node = document.find(nodeId_)) node->setName(oldName_);
    document.markDirty();
}

void ReparentNodeCommand::execute(SceneDocument& document) {
    Node* node = document.find(nodeId_);
    Node* newParent = document.find(newParentId_);
    if (!node || !node->parent() || !newParent) return;

    if (!initialized_) {
        oldParentId_ = node->parent()->id();
        oldIndex_ = childIndex(*node->parent(), node);
        if (newIndex_ == static_cast<size_t>(-1))
            newIndex_ = newParent->children().size();
        oldLocal_ = node->transform();
        newLocal_ = transformFromMatrix(glm::inverse(newParent->worldTransform()) * node->worldTransform());
        initialized_ = true;
    }

    auto owned = node->parent()->detachChild(node);
    if (!owned) return;
    owned->transform() = newLocal_;
    size_t insertIndex = newIndex_;
    if (oldParentId_ == newParentId_ && oldIndex_ < insertIndex)
        --insertIndex;
    newParent->addChildAt(std::move(owned), insertIndex);
    document.markDirty();
}

void ReparentNodeCommand::undo(SceneDocument& document) {
    Node* node = document.find(nodeId_);
    Node* oldParent = document.find(oldParentId_);
    if (!node || !node->parent() || !oldParent) return;
    auto owned = node->parent()->detachChild(node);
    if (!owned) return;
    owned->transform() = oldLocal_;
    oldParent->addChildAt(std::move(owned), oldIndex_);
    document.markDirty();
}

CreateParentCommand::CreateParentCommand(NodeId target, std::unique_ptr<Node> newParent)
    : targetId_(target),
      parentNodeId_(newParent ? newParent->id() : kNodeInvalid),
      pendingParent_(std::move(newParent)) {}

void CreateParentCommand::execute(SceneDocument& document) {
    Node* target = document.find(targetId_);
    if (!target || !target->parent() || !document.resources()) return;
    if (!initialized_) {
        oldParentId_ = target->parent()->id();
        targetIndex_ = childIndex(*target->parent(), target);
        originalTargetTransform_ = target->transform();
        initialized_ = true;
    }
    if (!pendingParent_ && !parentSnapshot_.empty())
        pendingParent_ = SceneSerializer::nodeFromJson(
            parentSnapshot_, *document.resources(), NodeIdPolicy::Preserve);
    Node* oldParent = document.find(oldParentId_);
    if (!oldParent || !pendingParent_) return;

    pendingParent_->transform() = originalTargetTransform_;
    target->transform() = Transform{};
    auto ownedTarget = oldParent->detachChild(target);
    if (!ownedTarget) return;
    Node* addedParent = oldParent->addChildAt(std::move(pendingParent_), targetIndex_);
    addedParent->addChild(std::move(ownedTarget));
    document.markDirty();
}

void CreateParentCommand::undo(SceneDocument& document) {
    Node* target = document.find(targetId_);
    Node* parentNode = document.find(parentNodeId_);
    Node* oldParent = document.find(oldParentId_);
    if (!target || !parentNode || !oldParent || !document.resources()) return;
    auto ownedTarget = parentNode->detachChild(target);
    if (!ownedTarget) return;
    parentSnapshot_ = SceneSerializer::nodeToJson(*parentNode, *document.resources());
    oldParent->removeChild(parentNode);
    ownedTarget->transform() = originalTargetTransform_;
    oldParent->addChildAt(std::move(ownedTarget), targetIndex_);
    document.markDirty();
}

void SetPropertyCommand::execute(SceneDocument& document) {
    if (Node* node = document.find(nodeId_)) applyNew_(*node);
    document.markDirty();
}

void SetPropertyCommand::undo(SceneDocument& document) {
    if (Node* node = document.find(nodeId_)) applyOld_(*node);
    document.markDirty();
}

void TransformCommand::execute(SceneDocument& document) {
    if (Node* node = document.find(nodeId_)) node->transform() = new_;
    document.markDirty();
}

void TransformCommand::undo(SceneDocument& document) {
    if (Node* node = document.find(nodeId_)) node->transform() = old_;
    document.markDirty();
}

void AddBehaviourCommand::execute(SceneDocument& document) {
    Node* node = document.find(nodeId_);
    if (!node) return;
    if (auto behaviour = BehaviourRegistry::instance().create(type_))
        node->addBehaviour(std::move(behaviour));
    document.markDirty();
}

void AddBehaviourCommand::undo(SceneDocument& document) {
    Node* node = document.find(nodeId_);
    if (!node) return;
    // Remove the last attached behaviour of this type (the one execute() added).
    Behaviour* victim = nullptr;
    for (const auto& b : node->behaviours())
        if (b->typeName() && type_ == b->typeName()) victim = b.get();
    if (victim) node->removeBehaviour(victim);
    document.markDirty();
}

void RemoveBehaviourCommand::execute(SceneDocument& document) {
    Node* node = document.find(nodeId_);
    if (!node) return;
    size_t seen = 0;
    for (const auto& b : node->behaviours()) {
        if (!b->typeName() || type_ != b->typeName()) continue;
        if (seen++ != index_) continue;

        if (snapshot_.empty()) {
            nlohmann::json j;
            j["type"] = type_;
            j["enabled"] = b->enabled();
            b->save(j);
            snapshot_ = j.dump();
        }
        node->removeBehaviour(b.get());
        document.markDirty();
        return;
    }
}

void RemoveBehaviourCommand::undo(SceneDocument& document) {
    Node* node = document.find(nodeId_);
    if (!node || snapshot_.empty()) return;
    nlohmann::json j = nlohmann::json::parse(snapshot_, nullptr, false);
    if (j.is_discarded()) return;
    auto behaviour = BehaviourRegistry::instance().create(type_);
    if (!behaviour) return;
    if (j.contains("enabled")) behaviour->setEnabled(j["enabled"].get<bool>());
    behaviour->load(j);
    node->addBehaviourAt(std::move(behaviour), index_);
    document.markDirty();
}

void AttachScriptCommand::execute(SceneDocument& document) {
    Node* node = document.find(nodeId_);
    if (!node) return;
    auto script = std::make_unique<ScriptBehaviour>();
    script->setScriptPath(scriptPath_);
    node->addBehaviour(std::move(script));
    document.markDirty();
}

void AttachScriptCommand::undo(SceneDocument& document) {
    Node* node = document.find(nodeId_);
    if (!node) return;
    Behaviour* victim = nullptr;
    for (const auto& b : node->behaviours())
        if (dynamic_cast<ScriptBehaviour*>(b.get())) victim = b.get();
    if (victim) node->removeBehaviour(victim);
    document.markDirty();
}

void ConnectSignalCommand::execute(SceneDocument& document) {
    if (Scene* scene = document.scene()) scene->connections().push_back(def_);
    document.markDirty();
}

void ConnectSignalCommand::undo(SceneDocument& document) {
    Scene* scene = document.scene();
    if (!scene) return;
    auto& defs = scene->connections();
    for (auto it = defs.end(); it != defs.begin();) {
        --it;
        if (it->from == def_.from && it->to == def_.to &&
            it->signal == def_.signal && it->slot == def_.slot) {
            defs.erase(it);
            break;
        }
    }
    document.markDirty();
}

} // namespace saida
