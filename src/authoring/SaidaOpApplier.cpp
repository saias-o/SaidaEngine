#include "authoring/SaidaOpApplier.hpp"

#include "authoring/EngineManifest.hpp"
#include "authoring/SaidaOp.hpp"
#include "core/Reflection.hpp"
#include "graphics/Material.hpp"
#include "graphics/ResourceManager.hpp"
#include "scene/Behaviour.hpp"
#include "scene/BehaviourRegistry.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/MeshNode.hpp"
#include "scene/Node.hpp"
#include "scene/NodeRegistry.hpp"
#include "nodes/ParticleSystemNode.hpp"
#include "scene/ReflectedTypes.hpp"
#include "scene/Scene.hpp"
#include "nodes/WaterNode.hpp"

#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace saida::authoring {
namespace {

using json = nlohmann::json;

Node* findById(Node& n, NodeId id) {
    if (n.id() == id) return &n;
    for (const auto& c : n.children())
        if (Node* r = findById(*c, id)) return r;
    return nullptr;
}

NodeId nodeId(const json& payload, const char* key) {
    return static_cast<NodeId>(std::stoull(payload.at(key).get<std::string>()));
}

std::string nodeIdText(NodeId id) { return std::to_string(id); }

std::string unknownNode(NodeId id) {
    return "unknown nodeId " + std::to_string(id);
}

glm::vec3 readVec3(const json& a, glm::vec3 fallback) {
    if (!a.is_array() || a.size() < 3) return fallback;
    return {a[0].get<float>(), a[1].get<float>(), a[2].get<float>()};
}
// Scene stores quaternions as [x, y, z, w]; glm::quat is (w, x, y, z).
glm::quat readQuat(const json& a, glm::quat fallback) {
    if (!a.is_array() || a.size() < 4) return fallback;
    return {a[3].get<float>(), a[0].get<float>(), a[1].get<float>(), a[2].get<float>()};
}

std::string err(const std::string& msg) {
    return json{{"ok", false}, {"error", msg}}.dump();
}
std::string ok(const std::string& applied, json diff) {
    return json{{"ok", true}, {"applied", applied}, {"diff", std::move(diff)}}.dump();
}
// The inverse carries undo/redo and the dry-run.
std::string ok(const std::string& applied, json diff, json inverse) {
    return json{{"ok", true}, {"applied", applied}, {"diff", std::move(diff)},
                {"inverse", std::move(inverse)}}.dump();
}
json inverseOp(const std::string& type, json payload) {
    return json{{"opVersion", kOpVersion}, {"type", type}, {"payload", std::move(payload)}};
}
json vec3Json(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
// Scene convention: quaternions as [x, y, z, w].
json quatJson(const glm::quat& q) { return json::array({q.x, q.y, q.z, q.w}); }

const reflect::TypeDesc* reflectedNodeDesc(Node& n) {
    const std::string type = n.typeName() ? n.typeName() : "";
    if (type == LightNode::reflectName()) return &reflect::localDesc<LightNode>();
    if (type == WaterNode::reflectName()) return &reflect::localDesc<WaterNode>();
    if (type == ParticleSystemNode::reflectName()) return &reflect::localDesc<ParticleSystemNode>();
    return nullptr;
}

std::string opSetTransform(Scene& scene, const json& p) {
    const NodeId target = nodeId(p, "nodeId");
    Node* n = findById(scene, target);
    if (!n) return err(unknownNode(target));
    json diff;
    json invPayload{{"nodeId", nodeIdText(target)}};  // only restores the fields that were touched
    if (p.contains("position")) {
        invPayload["position"] = vec3Json(n->transform().position);
        n->transform().position = readVec3(p["position"], n->transform().position);
        diff["position"] = p["position"];
    }
    if (p.contains("rotation")) {
        invPayload["rotation"] = quatJson(n->transform().rotation);
        n->transform().rotation = readQuat(p["rotation"], n->transform().rotation);
        diff["rotation"] = p["rotation"];
    }
    if (p.contains("scale")) {
        invPayload["scale"] = vec3Json(n->transform().scale);
        n->transform().scale = readVec3(p["scale"], n->transform().scale);
        diff["scale"] = p["scale"];
    }
    Node::g_transformVersion++;
    return ok("set_transform", diff, inverseOp("set_transform", std::move(invPayload)));
}

std::string opCreateNode(Scene& scene, ResourceManager* resources, const json& p) {
    const std::string type = p.value("nodeType", std::string("MeshNode"));
    const std::string name = p.value("name", std::string("Node"));
    const NodeId parentId = p.contains("parentId") ? nodeId(p, "parentId") : scene.id();

    Node* parent = findById(scene, parentId);
    if (!parent) return err("unknown parentId " + std::to_string(parentId));

    std::unique_ptr<Node> createdNode;

    if (type == "MeshNode") {
        // MeshNode needs resources unavailable to headless callers.
        if (!resources) return err("create_node MeshNode needs ResourceManager");
        Mesh* mesh = resources->getMesh(kAssetBuiltinCube);
        Material* mat = resources->getMaterial(MaterialDesc{});
        createdNode = std::make_unique<MeshNode>(name, mesh, mat);
    } else {
        registerReflectedTypes();  // idempotent: ensure the registry is populated
        std::unique_ptr<Node> node = NodeRegistry::instance().create(type);
        if (!node) return err("unsupported nodeType '" + type + "'");
        node->setName(name);
        createdNode = std::move(node);
    }
    if (p.contains("nodeId")) {
        const NodeId requestedId = nodeId(p, "nodeId");
        if (findById(scene, requestedId))
            return err("duplicate nodeId " + std::to_string(requestedId));
        createdNode->assignSerializedId(requestedId);
    }
    Node* created = parent->addChild(std::move(createdNode));
    if (!created) return err("failed to create node");
    Node::g_hierarchyVersion++;
    json inv = inverseOp("delete_node", json{{"nodeId", nodeIdText(created->id())}});
    return ok("create_node",
              json{{"nodeId", nodeIdText(created->id())}, {"name", created->name()},
                   {"nodeType", type}, {"parentId", nodeIdText(parent->id())}},
              std::move(inv));
}

std::string opDeleteNode(Scene& scene, const json& p) {
    const NodeId target = nodeId(p, "nodeId");
    Node* n = findById(scene, target);
    if (!n) return err(unknownNode(target));
    if (n == &scene) return err("cannot delete scene root");
    Node* parent = n->parent();
    if (!parent) return err("nodeId " + std::to_string(target) + " has no parent");
    parent->removeChild(n);
    Node::g_hierarchyVersion++;
    // Restoring a deleted subtree requires a snapshot.
    return ok("delete_node",
              json{{"deletedNodeId", nodeIdText(target)}, {"invertible", false}});
}

std::string opRenameNode(Scene& scene, const json& p) {
    const NodeId target = nodeId(p, "nodeId");
    const std::string newName = p.value("name", std::string());
    if (newName.empty()) return err("rename_node needs a non-empty name");
    Node* n = findById(scene, target);
    if (!n) return err(unknownNode(target));
    const std::string oldName = n->name();
    n->setName(newName);
    json inv = inverseOp("rename_node",
                         json{{"nodeId", nodeIdText(target)}, {"name", oldName}});
    return ok("rename_node",
              json{{"nodeId", nodeIdText(target)}, {"from", oldName}, {"to", newName}},
              std::move(inv));
}

std::string opReparentNode(Scene& scene, const json& p) {
    const NodeId target = nodeId(p, "nodeId");
    const NodeId parentId =
        p.contains("newParentId") ? nodeId(p, "newParentId") : scene.id();

    Node* n = findById(scene, target);
    if (!n) return err(unknownNode(target));
    if (n == &scene) return err("cannot reparent scene root");

    Node* newParent = findById(scene, parentId);
    if (!newParent) return err("unknown parentId " + std::to_string(parentId));
    if (newParent == n) return err("cannot reparent node under itself");
    for (Node* a = newParent->parent(); a; a = a->parent())
        if (a == n) return err("cannot reparent under a descendant (cycle)");

    Node* oldParent = n->parent();
    if (!oldParent) return err("nodeId " + std::to_string(target) + " has no parent");
    if (oldParent == newParent) {
        json inv = inverseOp("reparent_node",
                             json{{"nodeId", nodeIdText(target)},
                                  {"newParentId", nodeIdText(newParent->id())}});
        return ok("reparent_node",
                  json{{"nodeId", nodeIdText(target)}, {"unchanged", true}},
                  std::move(inv));
    }

    const NodeId oldParentId = oldParent->id();

    std::unique_ptr<Node> owned = oldParent->detachChild(n);
    if (!owned) return err("failed to detach nodeId " + std::to_string(target));
    newParent->addChild(std::move(owned));
    Node::g_hierarchyVersion++;
    json inv = inverseOp("reparent_node",
                         json{{"nodeId", nodeIdText(target)},
                              {"newParentId", nodeIdText(oldParentId)}});
    return ok("reparent_node", json{{"nodeId", nodeIdText(target)},
                                    {"fromParentId", nodeIdText(oldParentId)},
                                    {"toParentId", nodeIdText(newParent->id())}},
              std::move(inv));
}

std::string opSetProperty(Scene& scene, const json& p) {
    const NodeId target = nodeId(p, "nodeId");
    const std::string prop = p.value("property", std::string());
    Node* n = findById(scene, target);
    if (!n) return err(unknownNode(target));
    if (!p.contains("value")) return err("set_property needs a 'value'");
    const json& v = p["value"];

    auto setPropInverse = [&](const json& before) {
        return inverseOp("set_property", json{{"nodeId", nodeIdText(target)},
                                              {"property", prop}, {"value", before}});
    };

    if (prop == "name") {
        if (!v.is_string()) return err("property 'name' expects string");
        const std::string before = n->name();
        n->setName(v.get<std::string>());
        return ok("set_property", json{{"nodeId", nodeIdText(target)}, {"property", prop},
                                      {"before", before}, {"after", n->name()}},
                  setPropInverse(before));
    }
    if (prop == "enabled") {
        if (!v.is_boolean()) return err("property 'enabled' expects bool");
        const bool before = n->enabled();
        n->setEnabled(v.get<bool>());
        return ok("set_property", json{{"nodeId", nodeIdText(target)}, {"property", prop},
                                      {"before", before}, {"after", n->enabled()}},
                  setPropInverse(before));
    }

    const reflect::TypeDesc* desc = reflectedNodeDesc(*n);
    if (!desc) {
        return err("node type '" + std::string(n->typeName() ? n->typeName() : "<unknown>") +
                   "' has no reflected authoring properties");
    }
    const reflect::PropertyDesc* reflected = desc->findProperty(prop);
    if (!reflected) {
        return err("unknown reflected property '" + prop + "' on " + desc->name);
    }

    std::string why;
    if (!reflect::valueMatchesKind(*reflected, v, why)) {
        return err("property '" + prop + "' expects " + reflected->kind + " (" + why + ")");
    }

    json before;
    try {
        reflected->get(n, before);
        reflected->set(n, v);
    } catch (const std::exception& e) {
        return err("failed to set reflected property '" + prop + "': " + std::string(e.what()));
    }
    json after;
    reflected->get(n, after);
    json inv = setPropInverse(before);
    return ok("set_property", json{{"nodeId", nodeIdText(target)}, {"nodeType", n->typeName()},
                                  {"property", prop}, {"kind", reflected->kind},
                                  {"before", std::move(before)}, {"after", std::move(after)}},
              std::move(inv));
}

// set_scene_setting: edits scene ambiance (SceneSettings) by name. A curated
// subset relevant to the editor (fog/bloom/ambient/ibl/gi/skybox).
// Returns a re-applicable inverse (undo). Colors are stored as vec4
// but edited as vec3 (xyz), w preserved.
std::string opSetSceneSetting(Scene& scene, const json& p) {
    const std::string key = p.value("setting", std::string());
    if (key.empty()) return err("set_scene_setting needs 'setting'");
    if (!p.contains("value")) return err("set_scene_setting needs a 'value'");
    const json& v = p["value"];
    SceneSettings& s = scene.settings();

    auto sceneInverse = [&](const json& before) {
        return inverseOp("set_scene_setting", json{{"setting", key}, {"value", before}});
    };
    auto okScene = [&](const json& before, const json& after) {
        return ok("set_scene_setting",
                  json{{"setting", key}, {"before", before}, {"after", after}},
                  sceneInverse(before));
    };
    auto setFloat = [&](float& field) -> std::string {
        if (!v.is_number()) return err("scene setting '" + key + "' expects a number");
        const float before = field;
        field = v.get<float>();
        return okScene(before, field);
    };
    auto setBool = [&](bool& field) -> std::string {
        if (!v.is_boolean()) return err("scene setting '" + key + "' expects a bool");
        const bool before = field;
        field = v.get<bool>();
        return okScene(before, field);
    };
    // Colour stored as vec4; edited as a vec3, alpha preserved.
    auto setColor = [&](glm::vec4& field) -> std::string {
        if (!v.is_array() || v.size() != 3) return err("scene setting '" + key + "' expects a vec3");
        const json before = json::array({field.x, field.y, field.z});
        field.x = v[0].get<float>();
        field.y = v[1].get<float>();
        field.z = v[2].get<float>();
        return okScene(before, json::array({field.x, field.y, field.z}));
    };

    if (key == "fogEnabled") return setBool(s.fogEnabled);
    if (key == "bloomEnabled") return setBool(s.bloomEnabled);
    if (key == "aoEnabled") return setBool(s.aoEnabled);
    if (key == "iblEnabled") return setBool(s.iblEnabled);
    if (key == "giEnabled") return setBool(s.giEnabled);
    if (key == "enablePostProcessing") return setBool(s.enablePostProcessing);

    if (key == "fogStart") return setFloat(s.fogStart);
    if (key == "fogDensity") return setFloat(s.fogDensity);
    if (key == "bloomThreshold") return setFloat(s.bloomThreshold);
    if (key == "bloomIntensity") return setFloat(s.bloomIntensity);
    if (key == "bloomRadius") return setFloat(s.bloomRadius);
    if (key == "aoRadius") return setFloat(s.aoRadius);
    if (key == "aoIntensity") return setFloat(s.aoIntensity);
    if (key == "aoPower") return setFloat(s.aoPower);
    if (key == "giIntensity") return setFloat(s.giIntensity);
    if (key == "iblDiffuseIntensity") return setFloat(s.iblDiffuseIntensity);
    if (key == "iblSpecularIntensity") return setFloat(s.iblSpecularIntensity);
    if (key == "skyboxExposure") return setFloat(s.skyboxExposure);
    if (key == "skyboxRotation") return setFloat(s.skyboxRotation);

    if (key == "ambientLight") return setColor(s.ambientLight);
    if (key == "clearColor") return setColor(s.clearColor);
    if (key == "fogColor") return setColor(s.fogColor);

    return err("unknown scene setting '" + key + "'");
}

// --- behaviours (gameplay logic) --------------------------------------------

Behaviour* findBehaviourByType(Node& n, const std::string& type) {
    for (const auto& b : n.behaviours())
        if (b->typeName() && type == b->typeName()) return b.get();
    return nullptr;
}

std::string opAddBehaviour(Scene& scene, const json& p) {
    const NodeId target = nodeId(p, "nodeId");
    const std::string btype = p.value("behaviourType", std::string());
    Node* n = findById(scene, target);
    if (!n) return err(unknownNode(target));
    if (btype.empty()) return err("add_behaviour needs 'behaviourType'");

    registerReflectedTypes();  // idempotent: ensure the factory registry is ready
    if (findBehaviourByType(*n, btype))
        return err("nodeId " + std::to_string(target) +
                   " already has behaviour '" + btype + "'");
    std::unique_ptr<Behaviour> b = BehaviourRegistry::instance().create(btype);
    if (!b) return err("unknown behaviour type '" + btype + "'");
    n->addBehaviour(std::move(b));

    json inv = inverseOp("remove_behaviour",
                         json{{"nodeId", nodeIdText(target)}, {"behaviourType", btype}});
    return ok("add_behaviour",
              json{{"nodeId", nodeIdText(target)}, {"behaviourType", btype}},
              std::move(inv));
}

std::string opRemoveBehaviour(Scene& scene, const json& p) {
    const NodeId target = nodeId(p, "nodeId");
    const std::string btype = p.value("behaviourType", std::string());
    Node* n = findById(scene, target);
    if (!n) return err(unknownNode(target));
    Behaviour* b = findBehaviourByType(*n, btype);
    if (!b) return err("nodeId " + std::to_string(target) +
                       " has no behaviour '" + btype + "'");
    n->removeBehaviour(b);
    // Restoring removed behaviour properties requires a snapshot.
    return ok("remove_behaviour",
              json{{"nodeId", nodeIdText(target)}, {"behaviourType", btype},
                   {"invertible", false}});
}

std::string opSetBehaviourProperty(Scene& scene, const json& p) {
    const NodeId target = nodeId(p, "nodeId");
    const std::string btype = p.value("behaviourType", std::string());
    const std::string prop = p.value("property", std::string());
    Node* n = findById(scene, target);
    if (!n) return err(unknownNode(target));
    if (!p.contains("value")) return err("set_behaviour_property needs a 'value'");
    const json& v = p["value"];

    registerReflectedTypes();
    Behaviour* b = findBehaviourByType(*n, btype);
    if (!b) return err("nodeId " + std::to_string(target) +
                       " has no behaviour '" + btype + "'");
    const reflect::TypeDesc* desc = reflect::TypeRegistry::instance().find(btype);
    if (!desc) return err("behaviour '" + btype + "' has no reflected contract");
    const reflect::PropertyDesc* reflected = desc->findProperty(prop);
    if (!reflected) return err("unknown behaviour property '" + prop + "' on " + btype);

    std::string why;
    if (!reflect::valueMatchesKind(*reflected, v, why))
        return err("property '" + prop + "' expects " + reflected->kind + " (" + why + ")");

    json before;
    try {
        reflected->get(b, before);
        reflected->set(b, v);
    } catch (const std::exception& e) {
        return err("failed to set behaviour property '" + prop + "': " + std::string(e.what()));
    }
    json after;
    reflected->get(b, after);
    json inv = inverseOp("set_behaviour_property",
                         json{{"nodeId", nodeIdText(target)}, {"behaviourType", btype},
                              {"property", prop}, {"value", before}});
    return ok("set_behaviour_property",
              json{{"nodeId", nodeIdText(target)}, {"behaviourType", btype}, {"property", prop},
                   {"kind", reflected->kind}, {"before", std::move(before)},
                   {"after", std::move(after)}},
              std::move(inv));
}

std::string opAddSignalConnection(Scene& scene, const json& p) {
    const NodeId fromId = nodeId(p, "fromNodeId");
    const std::string signal = p.value("signal", std::string());
    const NodeId toId = nodeId(p, "toNodeId");
    const std::string slot = p.value("slot", std::string());

    Node* from = findById(scene, fromId);
    if (!from) return err(unknownNode(fromId));
    Node* to = findById(scene, toId);
    if (!to) return err(unknownNode(toId));

    for (const auto& c : scene.connections()) {
        if (c.from == from->id() && c.to == to->id() && c.signal == signal && c.slot == slot)
            return err("signal connection already exists");
    }

    scene.connections().push_back(
        SignalConnectionDef{from->id(), signal, to->id(), slot});

    json diff{{"fromNodeId", nodeIdText(fromId)}, {"signal", signal},
              {"toNodeId", nodeIdText(toId)}, {"slot", slot}};
    return ok("add_signal_connection", diff,
              inverseOp("remove_signal_connection", diff));
}

std::string opRemoveSignalConnection(Scene& scene, const json& p) {
    const NodeId fromId = nodeId(p, "fromNodeId");
    const std::string signal = p.value("signal", std::string());
    const NodeId toId = nodeId(p, "toNodeId");
    const std::string slot = p.value("slot", std::string());

    Node* from = findById(scene, fromId);
    if (!from) return err(unknownNode(fromId));
    Node* to = findById(scene, toId);
    if (!to) return err(unknownNode(toId));

    auto& conns = scene.connections();
    for (auto it = conns.begin(); it != conns.end(); ++it) {
        if (it->from == from->id() && it->to == to->id() &&
            it->signal == signal && it->slot == slot) {
            conns.erase(it);
            json diff{{"fromNodeId", nodeIdText(fromId)}, {"signal", signal},
                      {"toNodeId", nodeIdText(toId)}, {"slot", slot}};
            return ok("remove_signal_connection", diff,
                      inverseOp("add_signal_connection", diff));
        }
    }
    return err("signal connection not found");
}

} // namespace

std::string applyOpJson(Scene& scene, ResourceManager* resources,
                        const std::string& opJson) {
    // Validate before mutating the scene.
    const SaidaOpParseResult parsed = parseSaidaOp(opJson);
    if (!parsed.ok) return err(parsed.error);
    const std::string shapeError = validateOpShape(parsed.op);
    if (!shapeError.empty()) return err(shapeError);

    const std::string& type = parsed.op.type;
    const json& p = parsed.op.payload;

    if (type == "set_transform") return opSetTransform(scene, p);
    if (type == "create_node")   return opCreateNode(scene, resources, p);
    if (type == "delete_node")   return opDeleteNode(scene, p);
    if (type == "rename_node")   return opRenameNode(scene, p);
    if (type == "reparent_node") return opReparentNode(scene, p);
    if (type == "set_property")  return opSetProperty(scene, p);
    if (type == "set_scene_setting") return opSetSceneSetting(scene, p);
    if (type == "add_behaviour") return opAddBehaviour(scene, p);
    if (type == "remove_behaviour") return opRemoveBehaviour(scene, p);
    if (type == "set_behaviour_property") return opSetBehaviourProperty(scene, p);
    if (type == "add_signal_connection") return opAddSignalConnection(scene, p);
    if (type == "remove_signal_connection") return opRemoveSignalConnection(scene, p);
    return err("op type '" + type + "' is registered but not implemented");
}

std::string applyOpJson(Scene& scene, ResourceManager& resources,
                        const std::string& opJson) {
    return applyOpJson(scene, &resources, opJson);
}

} // namespace saida::authoring
