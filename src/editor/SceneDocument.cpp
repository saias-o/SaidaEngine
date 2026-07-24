#include "editor/SceneDocument.hpp"

#include "editor/Command.hpp"
#include "editor/CommandHistory.hpp"
#include "graphics/ResourceManager.hpp"
#include "scene/Node.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSerializer.hpp"

#include <memory>
#include <utility>

namespace saida {

void SceneDocument::bind(Scene* scene, ResourceManager* resources) {
    scene_ = scene;
    resources_ = resources;
    if (selectedId_ != kNodeInvalid && !find(selectedId_)) clearSelection();
}

Node* SceneDocument::find(NodeId id) const {
    if (!scene_ || id == kNodeInvalid) return nullptr;
    Node* result = nullptr;
    scene_->traverse([&](Node& node, const glm::mat4&) {
        if (!result && node.id() == id) result = &node;
    });
    return result;
}

void SceneDocument::select(Node* node) {
    selectedId_ = node ? node->id() : kNodeInvalid;
}

void SceneDocument::markLoaded() {
    dirty_ = false;
    clearSelection();
}

bool SceneDocument::save(const std::string& path) {
    if (!scene_ || !resources_) return false;
    if (!SceneSerializer::saveToFile(*scene_, *resources_, path))
        return false;
    currentPath_ = path;
    markSaved();
    return true;
}

bool SceneDocument::load(const std::string& path) {
    if (!scene_ || !resources_) return false;
    if (!SceneSerializer::loadIntoScene(*scene_, *resources_, path))
        return false;
    currentPath_ = path;
    markLoaded();
    return true;
}

void SceneDocument::copy(Node* node) {
    if (node && resources_)
        clipboard_ = SceneSerializer::nodeToJson(*node, *resources_);
}

Node* SceneDocument::paste(Node* parent, CommandHistory& history) {
    if (clipboard_.empty() || !resources_ || !parent) return nullptr;
    auto node = SceneSerializer::nodeFromJson(clipboard_, *resources_);
    if (!node) return nullptr;

    const NodeId addedId = node->id();
    history.execute(
        std::make_unique<AddNodeCommand>(parent->id(), std::move(node)));
    select(find(addedId));
    return selectedNode();
}

Node* SceneDocument::duplicate(Node* node, CommandHistory& history) {
    if (!node || !resources_ || !node->parent()) return nullptr;
    const std::string json = SceneSerializer::nodeToJson(*node, *resources_);
    auto clone = SceneSerializer::nodeFromJson(json, *resources_);
    if (!clone) return nullptr;

    const NodeId addedId = clone->id();
    const NodeId parentId = node->parent()->id();
    history.execute(
        std::make_unique<AddNodeCommand>(parentId, std::move(clone)));
    select(find(addedId));
    return selectedNode();
}

void SceneDocument::clearSessionReferences() {
    clipboard_.clear();
    currentPath_.clear();
    clearSelection();
}

} // namespace saida
