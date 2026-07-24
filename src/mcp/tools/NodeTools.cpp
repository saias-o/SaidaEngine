#include "mcp/tools/Tools.hpp"

#include "core/Reflection.hpp"
#include "editor/Command.hpp"
#include "editor/SceneDocument.hpp"
#include "graphics/ResourceManager.hpp"
#include "mcp/tools/ToolRegistry.hpp"
#include "scene/BehaviourRegistry.hpp"
#include "scene/GLTFLoader.hpp"
#include "scene/NodeRegistry.hpp"
#include "scene/Scene.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace saida::mcp {
namespace {
namespace fs = std::filesystem;

const reflect::PropertyDesc* findProp(const std::string& typeName, const std::string& prop) {
    if (const auto* d = reflect::TypeRegistry::instance().find(typeName))
        return d->findProperty(prop);
    return nullptr;
}

// Apply a JSON value to a (node or behaviour) reflected property, re-resolving
// the target each time so the closure stays valid across scene reconstructions.
std::function<void(Node&)> makePropApply(bool onBehaviour, std::string behType,
                                         std::string prop, json value) {
    return [onBehaviour, behType, prop, value](Node& n) {
        if (onBehaviour) {
            for (const auto& b : n.behaviours()) {
                if (b->typeName() && behType == b->typeName()) {
                    if (const auto* p = findProp(behType, prop)) p->set(b.get(), value);
                    return;
                }
            }
        } else if (const auto* p = findProp(n.typeName(), prop)) {
            p->set(&n, value);
        }
    };
}

json readProp(const Node& n, bool onBehaviour, const std::string& behType, const std::string& prop) {
    json out;
    if (onBehaviour) {
        for (const auto& b : n.behaviours())
            if (b->typeName() && behType == b->typeName()) {
                if (const auto* p = findProp(behType, prop)) p->get(b.get(), out);
                return out;
            }
    } else if (const auto* p = findProp(n.typeName(), prop)) {
        p->get(&n, out);
    }
    return out;
}

json toolCreateNode(const ToolContext& ctx, const json& args) {
    requireEdit(ctx);
    if (!args.contains("type")) fail("missing 'type'");
    std::string type = args["type"].get<std::string>();
    auto node = NodeRegistry::instance().create(type);
    if (!node) fail("unknown node type '" + type + "'");
    if (args.contains("name")) node->setName(args["name"].get<std::string>());
    node->regenerateId();
    NodeId newId = node->id();

    NodeId parent = args.contains("parent") ? parseNodeId(args["parent"]) : ctx.scene->id();
    if (!ctx.doc->find(parent)) fail("no parent node with id " + idString(parent));

    ctx.exec(std::make_unique<AddNodeCommand>(parent, std::move(node)));
    return {{"id", idString(newId)}};
}

json toolDeleteNode(const ToolContext& ctx, const json& args) {
    requireEdit(ctx);
    Node* node = requireNode(ctx, args);
    ctx.exec(std::make_unique<DeleteNodeCommand>(node->id()));
    return {{"ok", true}};
}

json toolRenameNode(const ToolContext& ctx, const json& args) {
    requireEdit(ctx);
    Node* node = requireNode(ctx, args);
    if (!args.contains("name")) fail("missing 'name'");
    ctx.exec(std::make_unique<RenameNodeCommand>(node->id(), node->name(),
                                                 args["name"].get<std::string>()));
    return {{"ok", true}};
}

json toolReparent(const ToolContext& ctx, const json& args) {
    requireEdit(ctx);
    Node* node = requireNode(ctx, args);
    if (!args.contains("parent")) fail("missing 'parent'");
    NodeId parent = parseNodeId(args["parent"]);
    if (!ctx.doc->find(parent)) fail("no parent node with id " + idString(parent));
    ctx.exec(std::make_unique<ReparentNodeCommand>(node->id(), parent));
    return {{"ok", true}};
}

json toolSetTransform(const ToolContext& ctx, const json& args) {
    requireEdit(ctx);
    Node* node = requireNode(ctx, args);
    Transform oldT = node->transform();
    Transform newT = oldT;
    if (args.contains("position")) newT.position = vec3From(args["position"], oldT.position);
    if (args.contains("scale")) newT.scale = vec3From(args["scale"], oldT.scale);
    if (args.contains("rotation") && args["rotation"].is_array() && args["rotation"].size() == 4) {
        const auto& r = args["rotation"];
        newT.rotation = glm::quat(r[3].get<float>(), r[0].get<float>(),
                                  r[1].get<float>(), r[2].get<float>());
    }
    ctx.exec(std::make_unique<TransformCommand>(node->id(), oldT, newT));
    return {{"ok", true}};
}

json toolAddBehaviour(const ToolContext& ctx, const json& args) {
    requireEdit(ctx);
    Node* node = requireNode(ctx, args);
    if (!args.contains("type")) fail("missing 'type'");
    std::string type = args["type"].get<std::string>();
    if (!BehaviourRegistry::instance().create(type)) fail("unknown behaviour type '" + type + "'");
    ctx.exec(std::make_unique<AddBehaviourCommand>(node->id(), type));
    return {{"ok", true}};
}

json toolSetProperty(const ToolContext& ctx, const json& args) {
    requireEdit(ctx);
    Node* node = requireNode(ctx, args);
    if (!args.contains("name") || !args.contains("value")) fail("missing 'name'/'value'");
    std::string prop = args["name"].get<std::string>();
    bool onBehaviour = args.contains("behaviour");
    std::string behType = onBehaviour ? args["behaviour"].get<std::string>() : std::string();

    std::string typeKey = onBehaviour ? behType : std::string(node->typeName());
    if (!findProp(typeKey, prop))
        fail("type '" + typeKey + "' has no reflected property '" + prop + "'");

    json oldVal = readProp(*node, onBehaviour, behType, prop);
    json newVal = args["value"];
    std::string label = "Set " + prop;
    ctx.exec(std::make_unique<SetPropertyCommand>(
        node->id(), label,
        makePropApply(onBehaviour, behType, prop, oldVal),
        makePropApply(onBehaviour, behType, prop, newVal)));
    return {{"ok", true}};
}

json toolGroup(const ToolContext& ctx, const json& args, bool add) {
    requireEdit(ctx);
    Node* node = requireNode(ctx, args);
    if (!args.contains("group")) fail("missing 'group'");
    std::string group = args["group"].get<std::string>();
    NodeId id = node->id();
    auto apply = [group, add](Node& n) { if (add) n.addToGroup(group); else n.removeFromGroup(group); };
    auto undo = [group, add](Node& n) { if (add) n.removeFromGroup(group); else n.addToGroup(group); };
    ctx.exec(std::make_unique<SetPropertyCommand>(id, add ? "Add To Group" : "Remove From Group",
                                                  undo, apply));
    return {{"ok", true}};
}

// Push an arbitrary serialized config into a behaviour via its load() hook. This
// is how nested, data-driven behaviours (StateMachine, Blackboard,
// ScriptBehaviour properties) are authored in one call. Undoable.
json toolConfigureBehaviour(const ToolContext& ctx, const json& args) {
    requireEdit(ctx);
    Node* node = requireNode(ctx, args);
    if (!args.contains("behaviour") || !args.contains("data")) fail("missing 'behaviour'/'data'");
    std::string behType = args["behaviour"].get<std::string>();
    json newData = args["data"];

    json oldData;
    bool found = false;
    for (const auto& b : node->behaviours())
        if (b->typeName() && behType == b->typeName()) { b->save(oldData); found = true; break; }
    if (!found) fail("node has no behaviour of type '" + behType + "'");

    auto applyData = [behType](Node& n, const json& d) {
        for (const auto& b : n.behaviours())
            if (b->typeName() && behType == b->typeName()) { b->load(d); return; }
    };
    ctx.exec(std::make_unique<SetPropertyCommand>(
        node->id(), "Configure " + behType,
        [applyData, oldData](Node& n) { applyData(n, oldData); },
        [applyData, newData](Node& n) { applyData(n, newData); }));
    return {{"ok", true}};
}

// Import a generated .glb/.gltf as a node subtree, optionally with auto LODs.
// The external model generator (a separate MCP) produces the file; this brings it
// into the scene. Undoable; re-imported from the path on undo/redo.
json toolImportModel(const ToolContext& ctx, const json& args) {
    requireEdit(ctx);
    if (!args.contains("path")) fail("missing 'path'");
    if (!ctx.resources) fail("no resources bound");
    SandboxedPathResult modelPath = resolveToolPath(ctx, args["path"].get<std::string>());
    if (!fs::exists(modelPath.absolute)) fail("model not found: " + modelPath.absolute);

    auto node = std::make_unique<Node>(args.value("name", fs::path(modelPath.relative).stem().string()));
    node->setImportedFromPath(modelPath.relative);
    GLTFLoadOptions opts;
    opts.autoMeshLods = args.value("lod", true);  // autoLOD by default
    if (!GLTFLoader::load(modelPath.absolute, *node, *ctx.resources, opts))
        fail("failed to import model: " + modelPath.absolute);
    node->regenerateId();
    NodeId id = node->id();

    NodeId parent = args.contains("parent") ? parseNodeId(args["parent"]) : ctx.scene->id();
    if (!ctx.doc->find(parent)) fail("no parent node with id " + idString(parent));
    ctx.exec(std::make_unique<AddNodeCommand>(parent, std::move(node)));
    return {{"id", idString(id)}};
}


} // namespace

void registerNodeTools(ToolRegistry& registry) {
    registry.add("create_node",
                 "Create a node under {parent} (default: scene root). Returns the new id.",
                 objectSchema({{"type", stringSchema()}, {"name", stringSchema()},
                               {"parent", stringSchema()}}, {"type"}), toolCreateNode);
    registry.add("delete_node", "Delete a node (and its subtree).",
                 objectSchema({{"id", stringSchema()}}, {"id"}), toolDeleteNode);
    registry.add("rename_node", "Rename a node.",
                 objectSchema({{"id", stringSchema()}, {"name", stringSchema()}},
                              {"id", "name"}), toolRenameNode);
    registry.add("reparent_node", "Move a node under a new parent.",
                 objectSchema({{"id", stringSchema()}, {"parent", stringSchema()}},
                              {"id", "parent"}), toolReparent);
    registry.add("set_transform",
                 "Set position/rotation(quat xyzw)/scale on a node.",
                 objectSchema({{"id", stringSchema()},
                               {"position", json{{"type", "array"}}},
                               {"rotation", json{{"type", "array"}}},
                               {"scale", json{{"type", "array"}}}}, {"id"}),
                 toolSetTransform);
    registry.add("add_behaviour", "Attach a behaviour type to a node.",
                 objectSchema({{"id", stringSchema()}, {"type", stringSchema()}},
                              {"id", "type"}), toolAddBehaviour);
    registry.add("set_property",
                 "Set a reflected property. Omit 'behaviour' for a node property, or pass the behaviour type name to target a behaviour.",
                 objectSchema({{"id", stringSchema()}, {"behaviour", stringSchema()},
                               {"name", stringSchema()},
                               {"value", json{{"type", {"number", "string", "boolean", "array"}}}}},
                              {"id", "name", "value"}), toolSetProperty);
    registry.add("add_to_group", "Add a node to a group tag.",
                 objectSchema({{"id", stringSchema()}, {"group", stringSchema()}},
                              {"id", "group"}),
                 [](const ToolContext& context, const json& args) {
                     return toolGroup(context, args, true);
                 });
    registry.add("remove_from_group", "Remove a node from a group tag.",
                 objectSchema({{"id", stringSchema()}, {"group", stringSchema()}},
                              {"id", "group"}),
                 [](const ToolContext& context, const json& args) {
                     return toolGroup(context, args, false);
                 });
    registry.add("configure_behaviour",
                 "Push a full serialized config into a behaviour (StateMachine/Blackboard/ScriptBehaviour). {id, behaviour:'<TypeName>', data:{...}}.",
                 objectSchema({{"id", stringSchema()}, {"behaviour", stringSchema()},
                               {"data", json{{"type", "object"}}}},
                              {"id", "behaviour", "data"}), toolConfigureBehaviour);
    registry.add("import_model",
                 "Import a .glb/.gltf as a node subtree (autoLOD on by default). {path, parent?, name?, lod?}.",
                 objectSchema({{"path", stringSchema()}, {"parent", stringSchema()},
                               {"name", stringSchema()},
                               {"lod", json{{"type", "boolean"}}}}, {"path"}),
                 toolImportModel);
}

} // namespace saida::mcp
