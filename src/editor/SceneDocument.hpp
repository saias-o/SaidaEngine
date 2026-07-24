#pragma once

#include "scene/NodeId.hpp"

#include <cstdint>
#include <string>

namespace saida {

class Node;
class Scene;
class ResourceManager;
class CommandHistory;

// Owns the editor's stable scene/selection binding and serialized document
// state. Selection is stored as a NodeId so commands never retain raw pointers.
class SceneDocument {
public:
    void bind(Scene* scene, ResourceManager* resources);

    Scene* scene() const { return scene_; }
    ResourceManager* resources() const { return resources_; }
    Node* find(NodeId id) const;

    void select(Node* node);
    void clearSelection() { selectedId_ = kNodeInvalid; }
    NodeId selectedId() const { return selectedId_; }
    Node* selectedNode() const { return find(selectedId_); }

    bool dirty() const { return dirty_; }
    void markDirty() { dirty_ = true; }
    void markSaved() { dirty_ = false; }
    void markLoaded();  // clears dirty + selection

    bool save(const std::string& path);
    bool load(const std::string& path);

    void copy(Node* node);
    Node* paste(Node* parent, CommandHistory& history);
    Node* duplicate(Node* node, CommandHistory& history);
    bool hasClipboard() const { return !clipboard_.empty(); }

    const std::string& currentPath() const { return currentPath_; }
    void clearCurrentPath() { currentPath_.clear(); }
    void clearSessionReferences();

private:
    Scene* scene_ = nullptr;
    ResourceManager* resources_ = nullptr;
    NodeId selectedId_ = kNodeInvalid;
    bool dirty_ = false;
    std::string clipboard_;
    std::string currentPath_;
};

} // namespace saida
