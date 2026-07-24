#pragma once

#include "scene/Node.hpp"
#include "scene/SignalWiring.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace saida {

class ResourceManager;
class SceneDocument;

class Command {
public:
    virtual ~Command() = default;
    virtual void execute(SceneDocument& document) = 0;
    virtual void undo(SceneDocument& document) = 0;
    virtual const char* name() const { return "Command"; }
    virtual size_t memoryCost() const { return sizeof(*this); }
};

class AddNodeCommand : public Command {
public:
    AddNodeCommand(NodeId parent, std::unique_ptr<Node> node);
    void execute(SceneDocument& document) override;
    void undo(SceneDocument& document) override;
    const char* name() const override { return "Add Node"; }
    size_t memoryCost() const override { return sizeof(*this) + snapshot_.size(); }
    NodeId nodeId() const { return nodeId_; }

private:
    NodeId parentId_;
    NodeId nodeId_;
    std::unique_ptr<Node> pending_;
    std::string snapshot_;
};

class DeleteNodeCommand : public Command {
public:
    explicit DeleteNodeCommand(NodeId node) : nodeId_(node) {}
    void execute(SceneDocument& document) override;
    void undo(SceneDocument& document) override;
    const char* name() const override { return "Delete Node"; }
    size_t memoryCost() const override { return sizeof(*this) + snapshot_.size(); }

private:
    NodeId parentId_ = kNodeInvalid;
    NodeId nodeId_ = kNodeInvalid;
    size_t index_ = 0;
    std::string snapshot_;
};

class RenameNodeCommand : public Command {
public:
    RenameNodeCommand(NodeId node, std::string oldName, std::string newName)
        : nodeId_(node), oldName_(std::move(oldName)), newName_(std::move(newName)) {}
    void execute(SceneDocument& document) override;
    void undo(SceneDocument& document) override;
    const char* name() const override { return "Rename Node"; }
    size_t memoryCost() const override { return sizeof(*this) + oldName_.size() + newName_.size(); }

private:
    NodeId nodeId_;
    std::string oldName_;
    std::string newName_;
};

class ReparentNodeCommand : public Command {
public:
    ReparentNodeCommand(NodeId node, NodeId newParent, size_t newIndex = static_cast<size_t>(-1))
        : nodeId_(node), newParentId_(newParent), newIndex_(newIndex) {}
    void execute(SceneDocument& document) override;
    void undo(SceneDocument& document) override;
    const char* name() const override { return "Reparent Node"; }

private:
    NodeId nodeId_;
    NodeId oldParentId_ = kNodeInvalid;
    NodeId newParentId_;
    size_t oldIndex_ = 0;
    size_t newIndex_ = 0;
    Transform oldLocal_;
    Transform newLocal_;
    bool initialized_ = false;
};

class CreateParentCommand : public Command {
public:
    CreateParentCommand(NodeId target, std::unique_ptr<Node> newParent);
    void execute(SceneDocument& document) override;
    void undo(SceneDocument& document) override;
    const char* name() const override { return "Create Parent"; }
    size_t memoryCost() const override { return sizeof(*this) + parentSnapshot_.size(); }
    NodeId parentNodeId() const { return parentNodeId_; }

private:
    NodeId targetId_;
    NodeId oldParentId_ = kNodeInvalid;
    NodeId parentNodeId_ = kNodeInvalid;
    size_t targetIndex_ = 0;
    Transform originalTargetTransform_;
    std::unique_ptr<Node> pendingParent_;
    std::string parentSnapshot_;
    bool initialized_ = false;
};

// Generic single-property edit on one node. The before/after values are baked
// into the `apply` closures (captured by value), keeping the command fully
// type-erased: it re-resolves the node by id on every execute/undo, so it stays
// valid across scene reconstructions (following the NodeId policy). Used by the inspector
// property editor to make every field edit undoable and dirty-marking.
class SetPropertyCommand : public Command {
public:
    using Apply = std::function<void(Node&)>;
    SetPropertyCommand(NodeId node, std::string label, Apply applyOld, Apply applyNew)
        : nodeId_(node), label_(std::move(label)),
          applyOld_(std::move(applyOld)), applyNew_(std::move(applyNew)) {}
    void execute(SceneDocument& document) override;
    void undo(SceneDocument& document) override;
    const char* name() const override { return label_.c_str(); }
    size_t memoryCost() const override { return sizeof(*this) + label_.capacity(); }

private:
    NodeId nodeId_;
    std::string label_;
    Apply applyOld_;
    Apply applyNew_;
};

class TransformCommand : public Command {
public:
    TransformCommand(NodeId node, const Transform& oldT, const Transform& newT)
        : nodeId_(node), old_(oldT), new_(newT) {}
    void execute(SceneDocument& document) override;
    void undo(SceneDocument& document) override;
    const char* name() const override { return "Transform"; }

private:
    NodeId nodeId_;
    Transform old_;
    Transform new_;
};

// Attach a behaviour (by registered type name) to a node. Undo removes the last
// instance of that type. Used by the MCP `add_behaviour` tool.
class AddBehaviourCommand : public Command {
public:
    AddBehaviourCommand(NodeId node, std::string behaviourType)
        : nodeId_(node), type_(std::move(behaviourType)) {}
    void execute(SceneDocument& document) override;
    void undo(SceneDocument& document) override;
    const char* name() const override { return "Add Behaviour"; }

private:
    NodeId nodeId_;
    std::string type_;
};

class RemoveBehaviourCommand : public Command {
public:
    RemoveBehaviourCommand(NodeId node, std::string behaviourType, size_t index)
        : nodeId_(node), type_(std::move(behaviourType)), index_(index) {}
    void execute(SceneDocument& document) override;
    void undo(SceneDocument& document) override;
    const char* name() const override { return "Remove Behaviour"; }
    size_t memoryCost() const override { return sizeof(*this) + type_.capacity() + snapshot_.capacity(); }

private:
    NodeId nodeId_;
    std::string type_;
    size_t index_ = 0;
    std::string snapshot_;
};

// Attach a ScriptBehaviour pointing at a JS file to a node. Undo removes the last
// ScriptBehaviour. Used by the MCP `write_script` tool's optional attach step.
class AttachScriptCommand : public Command {
public:
    AttachScriptCommand(NodeId node, std::string scriptPath)
        : nodeId_(node), scriptPath_(std::move(scriptPath)) {}
    void execute(SceneDocument& document) override;
    void undo(SceneDocument& document) override;
    const char* name() const override { return "Attach Script"; }

private:
    NodeId nodeId_;
    std::string scriptPath_;
};

// Append a data-driven signal→slot link to the scene's connections. Used by the
// MCP `connect_signal` tool.
class ConnectSignalCommand : public Command {
public:
    explicit ConnectSignalCommand(SignalConnectionDef def) : def_(std::move(def)) {}
    void execute(SceneDocument& document) override;
    void undo(SceneDocument& document) override;
    const char* name() const override { return "Connect Signal"; }

private:
    SignalConnectionDef def_;
};

} // namespace saida
