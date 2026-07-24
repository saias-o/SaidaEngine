#include "authoring/EngineManifest.hpp"
#include "authoring/SaidaOp.hpp"
#include "authoring/SceneSnapshot.hpp"
#include "authoring/SaidaOpApplier.hpp"
#include "core/FormatVersions.hpp"
#include "physics/AreaNode.hpp"
#include "nodes/CameraNode.hpp"
#include "scene/BehaviourRegistry.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/MeshNode.hpp"
#include "scene/NodeRegistry.hpp"
#include "nodes/ParticleSystemNode.hpp"
#include "behaviours/RotatorBehaviour.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSerializer.hpp"
#include "nodes/WaterNode.hpp"
#include "scripting/ScriptBehaviour.hpp"

#include <nlohmann/json.hpp>
#include <glm/glm.hpp>

#include <cmath>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>

using nlohmann::json;

namespace {

void require(bool condition) {
    if (!condition) std::abort();
}

json applyOp(saida::Scene& scene, const json& op) {
    return json::parse(saida::authoring::applyOpJson(scene, nullptr, op.dump()));
}

std::string nodeRef(saida::NodeId id) { return std::to_string(id); }

json setProperty(saida::NodeId nodeId, const std::string& property, json value) {
    return {
        {"type", "set_property"},
        {"payload", {
            {"nodeId", nodeRef(nodeId)},
            {"property", property},
            {"value", std::move(value)},
        }},
    };
}

bool close(float a, float b) {
    return std::abs(a - b) < 0.0001f;
}

// Direct search among children (tests reference by name).
saida::Node* findChildByName(saida::Node& parent, const std::string& name) {
    for (const auto& c : parent.children())
        if (c->name() == name) return c.get();
    return nullptr;
}

void testReflectedLightProperty() {
    saida::Scene scene;
    auto light = std::make_unique<saida::LightNode>("Sun", saida::LightType::Directional);
    saida::LightNode* sun = light.get();
    scene.addChild(std::move(light));

    json res = applyOp(scene, setProperty(sun->id(), "intensity", 3.5f));
    require(res["ok"].get<bool>());
    require(close(sun->intensity, 3.5f));
    require(res["diff"]["before"].get<float>() == 1.0f);
    require(res["diff"]["after"].get<float>() == 3.5f);

    const glm::vec3 beforeColor = sun->color;
    res = applyOp(scene, setProperty(sun->id(), "color", true));
    require(!res["ok"].get<bool>());
    require(sun->color == beforeColor);

    const saida::LightType beforeType = sun->type;
    res = applyOp(scene, setProperty(sun->id(), "lightType", 99));
    require(!res["ok"].get<bool>());
    require(sun->type == beforeType);
}

void testReflectedWaterAndParticleProperties() {
    saida::Scene scene;
    auto water = std::make_unique<saida::WaterNode>();
    water->setName("Sea");
    saida::WaterNode* sea = water.get();
    scene.addChild(std::move(water));

    auto particles = std::make_unique<saida::ParticleSystemNode>();
    particles->setName("Sparks");
    saida::ParticleSystemNode* sparks = particles.get();
    scene.addChild(std::move(particles));

    json res = applyOp(scene, setProperty(sea->id(), "amplitude", 0.75f));
    require(res["ok"].get<bool>());
    require(close(sea->amplitude, 0.75f));

    res = applyOp(scene, setProperty(sparks->id(), "maxParticles", 1024));
    require(res["ok"].get<bool>());
    require(sparks->maxParticles == 1024);

    res = applyOp(scene, setProperty(sparks->id(), "maxParticles", 12.5f));
    require(!res["ok"].get<bool>());
    require(sparks->maxParticles == 1024);
}

void testInvalidOpsDoNotMutate() {
    saida::Scene scene;
    auto light = std::make_unique<saida::LightNode>("Lamp", saida::LightType::Point);
    saida::LightNode* lamp = light.get();
    scene.addChild(std::move(light));

    json res = applyOp(scene, setProperty(999999, "intensity", 4.0f));
    require(!res["ok"].get<bool>());
    require(close(lamp->intensity, 1.0f));

    res = applyOp(scene, setProperty(lamp->id(), "wobble", 4.0f));
    require(!res["ok"].get<bool>());
    require(close(lamp->intensity, 1.0f));

    res = applyOp(scene, setProperty(lamp->id(), "name", false));
    require(!res["ok"].get<bool>());
    require(lamp->name() == "Lamp");

    res = applyOp(scene, json{{"type", "rename_node"}, {"payload", {{"nodeId", nodeRef(lamp->id())}, {"name", ""}}}});
    require(!res["ok"].get<bool>());
    require(lamp->name() == "Lamp");

    res = applyOp(scene, json{{"type", "delete_node"}, {"payload", {{"nodeId", nodeRef(scene.id())}}}});
    require(!res["ok"].get<bool>());
    require(!scene.children().empty());
}

void testCreateNodeResourceBoundaries() {
    saida::Scene scene;

    json res = applyOp(scene, json{{"type", "create_node"},
                                   {"payload", {{"nodeType", "LightNode"}, {"name", "AddedLamp"}}}});
    require(res["ok"].get<bool>());
    require(scene.children().size() == 1);
    require(scene.children()[0]->name() == "AddedLamp");

    res = applyOp(scene, json{{"type", "create_node"},
                              {"payload", {{"nodeType", "MeshNode"}, {"name", "NeedsGpu"}}}});
    require(!res["ok"].get<bool>());
    require(scene.children().size() == 1);

    // Reflected node palette via NodeRegistry (no GPU): Water, ParticleSystem.
    res = applyOp(scene, json{{"type", "create_node"},
                              {"payload", {{"nodeType", "Water"}, {"name", "Sea"}}}});
    require(res["ok"].get<bool>());
    saida::Node* sea = findChildByName(scene, "Sea");
    require(sea != nullptr && dynamic_cast<saida::WaterNode*>(sea) != nullptr);

    res = applyOp(scene, json{{"type", "create_node"},
                              {"payload", {{"nodeType", "ParticleSystem"}, {"name", "Sparks"}}}});
    require(res["ok"].get<bool>());
    require(dynamic_cast<saida::ParticleSystemNode*>(findChildByName(scene, "Sparks")) != nullptr);

    // A batch can supply the ID so that subsequent operations can target it.
    res = applyOp(scene, json{{"type", "create_node"},
                              {"payload", {{"nodeType", "LightNode"}, {"nodeId", "4242"},
                                           {"name", "BatchNode"}}}});
    require(res["ok"].get<bool>());
    require(res["diff"]["nodeId"] == "4242");
    res = applyOp(scene, json{{"type", "create_node"},
                              {"payload", {{"nodeType", "LightNode"}, {"nodeId", "4242"},
                                           {"name", "DuplicateId"}}}});
    require(!res["ok"].get<bool>());

    // A property set on a freshly-created reflected node applies.
    res = applyOp(scene, setProperty(sea->id(), "amplitude", 0.5f));
    require(res["ok"].get<bool>());

    // Unknown types are still rejected.
    res = applyOp(scene, json{{"type", "create_node"},
                              {"payload", {{"nodeType", "Wobbulator"}, {"name", "X"}}}});
    require(!res["ok"].get<bool>());
}

// Phase A1: the SaidaOp contract is versioned, serializable, round-trip stable.
void testSaidaOpRoundTrip() {
    const json in = {
        {"type", "set_transform"},
        {"sceneId", "main"},
        {"payload", {{"nodeId", "42"}, {"position", {4.0f, 0.6f, 12.0f}}}},
    };
    auto parsed = saida::authoring::parseSaidaOp(in);
    require(parsed.ok);
    require(parsed.op.opVersion == saida::authoring::kOpVersion);  // default filled in
    require(parsed.op.type == "set_transform");
    require(parsed.op.sceneId == "main");

    // parse(toJson(op)) == op, and toJson is stable (double round-trip).
    const json out = parsed.op.toJson();
    auto reparsed = saida::authoring::parseSaidaOp(out);
    require(reparsed.ok);
    require(reparsed.op.opVersion == parsed.op.opVersion);
    require(reparsed.op.type == parsed.op.type);
    require(reparsed.op.sceneId == parsed.op.sceneId);
    require(reparsed.op.payload == parsed.op.payload);
    require(reparsed.op.toJson() == out);

    // Empty sceneId is not emitted.
    auto minimal = saida::authoring::parseSaidaOp(json{{"type", "delete_node"}});
    require(minimal.ok);
    require(!minimal.op.toJson().contains("sceneId"));
    require(minimal.op.toJson()["payload"].is_object());
}

// Phase A4: table of shape rejections -- none should pass the parse.
void testSaidaOpParseRejections() {
    using saida::authoring::parseSaidaOp;
    require(!parseSaidaOp(std::string("not json at all")).ok);
    require(!parseSaidaOp(json::array({1, 2, 3})).ok);                  // not an object
    require(!parseSaidaOp(json{{"payload", json::object()}}).ok);       // missing type
    require(!parseSaidaOp(json{{"type", 42}}).ok);                      // type not a string
    require(!parseSaidaOp(json{{"type", "explode_scene"}}).ok);         // unknown type
    require(!parseSaidaOp(json{{"type", "delete_node"}, {"opVersion", 999}}).ok);
    require(!parseSaidaOp(json{{"type", "delete_node"}, {"opVersion", "1"}}).ok);
    require(!parseSaidaOp(json{{"type", "delete_node"}, {"payload", "x"}}).ok);
    require(!parseSaidaOp(json{{"type", "delete_node"}, {"sceneId", 7}}).ok);

    // The applier refuses the same shapes without mutating the scene.
    saida::Scene scene;
    saida::Node* keep = scene.addChild(
        std::make_unique<saida::LightNode>("Keep", saida::LightType::Point));
    json res = applyOp(scene, json{{"type", "explode_scene"}, {"payload", json::object()}});
    require(!res["ok"].get<bool>());
    res = applyOp(scene, json{{"type", "delete_node"}, {"opVersion", 999},
                              {"payload", {{"nodeId", nodeRef(keep->id())}}}});
    require(!res["ok"].get<bool>());
    require(scene.children().size() == 1);
}

// Phase A4: reparent_node -- protected tree structure (root, self, cycles).
void testReparentNode() {
    saida::Scene scene;
    auto* a = scene.createChild<saida::Node>("A");
    auto* b = scene.createChild<saida::Node>("B");
    auto* c = a->createChild<saida::Node>("C");
    (void)b;

    // Valid move: C goes from A to B.
    json res = applyOp(scene, json{{"type", "reparent_node"},
                                   {"payload", {{"nodeId", nodeRef(c->id())},
                                                {"newParentId", nodeRef(b->id())}}}});
    require(res["ok"].get<bool>());
    require(c->parent() == b);
    require(a->children().empty());
    require(res["diff"]["fromParentId"] == nodeRef(a->id()));
    require(res["diff"]["toParentId"] == nodeRef(b->id()));

    // Missing newParentId = scene root.
    res = applyOp(scene, json{{"type", "reparent_node"},
                              {"payload", {{"nodeId", nodeRef(c->id())}}}});
    require(res["ok"].get<bool>());
    require(c->parent() == &scene);

    // Same parent: accepted, marked unchanged, no mutation.
    res = applyOp(scene, json{{"type", "reparent_node"},
                              {"payload", {{"nodeId", nodeRef(c->id())},
                                           {"newParentId", nodeRef(scene.id())}}}});
    require(res["ok"].get<bool>());
    require(res["diff"]["unchanged"].get<bool>());

    // Rejections: root, self, descendant (cycle), unknown.
    res = applyOp(scene, json{{"type", "reparent_node"},
                              {"payload", {{"nodeId", nodeRef(scene.id())},
                                           {"newParentId", nodeRef(b->id())}}}});
    require(!res["ok"].get<bool>());
    res = applyOp(scene, json{{"type", "reparent_node"},
                              {"payload", {{"nodeId", nodeRef(a->id())},
                                           {"newParentId", nodeRef(a->id())}}}});
    require(!res["ok"].get<bool>());
    auto* d = a->createChild<saida::Node>("D");
    res = applyOp(scene, json{{"type", "reparent_node"},
                              {"payload", {{"nodeId", nodeRef(a->id())},
                                           {"newParentId", nodeRef(d->id())}}}});
    require(!res["ok"].get<bool>());
    require(d->parent() == a);
    res = applyOp(scene, json{{"type", "reparent_node"},
                              {"payload", {{"nodeId", "999998"},
                                           {"newParentId", nodeRef(b->id())}}}});
    require(!res["ok"].get<bool>());
    res = applyOp(scene, json{{"type", "reparent_node"},
                              {"payload", {{"nodeId", nodeRef(d->id())},
                                           {"newParentId", "999999"}}}});
    require(!res["ok"].get<bool>());
    require(d->parent() == a);
}

// Phase B2: static shape validation (without a scene) -- dry-run.
void testValidateOpShape() {
    using saida::authoring::parseSaidaOp;
    using saida::authoring::validateOpShape;

    auto shapeOf = [](const json& j) -> std::string {
        auto r = parseSaidaOp(j);
        if (!r.ok) return r.error;               // already rejected at parse time
        return validateOpShape(r.op);            // "" if the shape is valid
    };

    // Valid shapes -> empty string.
    require(shapeOf(json{{"type", "set_transform"},
                         {"payload", {{"nodeId", "1"}, {"position", {1, 2, 3}}}}}).empty());
    require(shapeOf(json{{"type", "reparent_node"},
                         {"payload", {{"nodeId", "1"}}}}).empty());  // newParentId optional
    require(shapeOf(json{{"type", "set_property"},
                         {"payload", {{"nodeId", "1"}, {"property", "intensity"}, {"value", 2}}}}).empty());
    require(shapeOf(json{{"type", "create_node"},
                         {"payload", {{"nodeType", "LightNode"}}}}).empty());

    // Invalid shapes -> non-empty message.
    require(!shapeOf(json{{"type", "set_transform"}, {"payload", {{"nodeId", "1"}}}}).empty()); // no field
    require(!shapeOf(json{{"type", "set_transform"},
                          {"payload", {{"nodeId", "1"}, {"position", {1, 2}}}}}).empty());      // truncated vec3
    require(!shapeOf(json{{"type", "set_transform"},
                          {"payload", {{"nodeId", "0"}, {"position", {1, 2, 3}}}}}).empty());    // invalid nodeId
    require(!shapeOf(json{{"type", "rename_node"}, {"payload", {{"nodeId", "1"}}}}).empty());   // missing name
    require(!shapeOf(json{{"type", "set_property"},
                          {"payload", {{"nodeId", "1"}, {"property", "x"}}}}).empty());          // missing value
    require(!shapeOf(json{{"type", "delete_node"},
                          {"payload", {{"nodeId", 1}}}}).empty());  // JSON numbers forbidden
    require(!shapeOf(json{{"type", "create_node"}, {"payload", json::object()}}).empty());       // missing nodeType
    // Unknown type: rejected at parse time.
    require(!shapeOf(json{{"type", "explode_scene"}, {"payload", json::object()}}).empty());
}

void testManifestListsRegistryOps() {
    json manifest = saida::authoring::buildEngineManifest();
    const auto& types = saida::authoring::knownOpTypes();
    require(manifest["ops"].size() == types.size());
    for (const auto& t : types) {
        bool found = false;
        for (const auto& op : manifest["ops"])
            if (op == t) found = true;
        require(found);
    }
    require(saida::authoring::isKnownOpType("reparent_node"));
    require(!saida::authoring::isKnownOpType("explode_scene"));
}

// Phase A5: every mutating op returns a re-applicable inverse
// (apply -> invert restores the state). Foundation for undo/redo + AI dry-run.
void testInverseOps() {
    saida::Scene scene;
    auto* a = scene.createChild<saida::Node>("A");
    auto* b = scene.createChild<saida::Node>("B");
    auto* c = a->createChild<saida::Node>("C");
    auto light = std::make_unique<saida::LightNode>("Sun", saida::LightType::Directional);
    saida::LightNode* sun = light.get();
    scene.addChild(std::move(light));
    (void)b; (void)c;

    // set_transform: position restored after apply + apply(inverse).
    a->transform().position = {1.0f, 2.0f, 3.0f};
    json res = applyOp(scene, json{{"type", "set_transform"},
                                   {"payload", {{"nodeId", nodeRef(a->id())},
                                                {"position", {9.0f, 9.0f, 9.0f}}}}});
    require(res["ok"].get<bool>());
    require(res.contains("inverse"));
    require(close(a->transform().position.x, 9.0f));
    json inv = applyOp(scene, res["inverse"]);
    require(inv["ok"].get<bool>());
    require(close(a->transform().position.x, 1.0f));
    require(close(a->transform().position.y, 2.0f));
    require(close(a->transform().position.z, 3.0f));

    // Reflected set_property: intensity restored.
    res = applyOp(scene, setProperty(sun->id(), "intensity", 5.0f));
    require(res["ok"].get<bool>());
    require(close(sun->intensity, 5.0f));
    applyOp(scene, res["inverse"]);
    require(close(sun->intensity, 1.0f));

    // set_property name: the inverse keeps the same stable NodeId.
    res = applyOp(scene, setProperty(sun->id(), "name", "Star"));
    require(res["ok"].get<bool>());
    require(sun->name() == "Star");
    require(res["inverse"]["payload"]["nodeId"] == nodeRef(sun->id()));
    applyOp(scene, res["inverse"]);
    require(sun->name() == "Sun");

    // rename_node: same idea, inverse restores the original name.
    res = applyOp(scene, json{{"type", "rename_node"},
                              {"payload", {{"nodeId", nodeRef(a->id())}, {"name", "Alpha"}}}});
    require(res["ok"].get<bool>());
    require(a->name() == "Alpha");
    applyOp(scene, res["inverse"]);
    require(a->name() == "A");

    // reparent_node: C moves back under A after apply(inverse).
    res = applyOp(scene, json{{"type", "reparent_node"},
                              {"payload", {{"nodeId", nodeRef(c->id())},
                                           {"newParentId", nodeRef(b->id())}}}});
    require(res["ok"].get<bool>());
    require(c->parent() == b);
    applyOp(scene, res["inverse"]);
    require(c->parent() == a);

    // create_node: inverse = delete_node of the created node.
    res = applyOp(scene, json{{"type", "create_node"},
                              {"payload", {{"nodeType", "LightNode"}, {"name", "Temp"}}}});
    require(res["ok"].get<bool>());
    require(res["inverse"]["type"] == "delete_node");
    require(findChildByName(scene, "Temp") != nullptr);
    applyOp(scene, res["inverse"]);
    require(findChildByName(scene, "Temp") == nullptr);

    // delete_node: honestly marked as non-op-invertible (restore via snapshot).
    res = applyOp(scene, json{{"type", "delete_node"},
                              {"payload", {{"nodeId", nodeRef(b->id())}}}});
    require(res["ok"].get<bool>());
    require(!res.contains("inverse"));
    require(res["diff"]["invertible"].get<bool>() == false);
}

void testStableNodeIdAddressing() {
    saida::Scene scene;
    auto* first = scene.createChild<saida::Node>("Duplicate");
    auto* second = scene.createChild<saida::Node>("Duplicate");
    const saida::NodeId firstId = first->id();
    const saida::NodeId secondId = second->id();

    json res = applyOp(scene, json{{"type", "rename_node"},
                                   {"payload", {{"nodeId", nodeRef(secondId)},
                                                {"name", "Renamed"}}}});
    require(res["ok"].get<bool>());
    require(first->name() == "Duplicate");
    require(second->name() == "Renamed");
    require(second->id() == secondId);

    res = applyOp(scene, json{{"type", "set_transform"},
                              {"payload", {{"nodeId", nodeRef(secondId)},
                                           {"position", {7.0f, 8.0f, 9.0f}}}}});
    require(res["ok"].get<bool>());
    require(close(second->transform().position.x, 7.0f));
    require(close(first->transform().position.x, 0.0f));

    // A name, even a unique one, is no longer a valid reference in opVersion 2.
    res = applyOp(scene, json{{"type", "set_transform"},
                              {"payload", {{"nodeId", "Renamed"},
                                           {"position", {1.0f, 2.0f, 3.0f}}}}});
    require(!res["ok"].get<bool>());
    require(first->id() == firstId);
}

void testManifestContainsReflectedProperties() {
    json manifest = saida::authoring::buildEngineManifest();
    json manifestAgain = saida::authoring::buildEngineManifest();
    require(manifest["engineVersion"] == saida::authoring::kEngineVersion);
    require(manifest["engineVersion"] == "1.0.0");
    require(manifest["opVersion"].get<int>() == saida::authoring::kOpVersion);
    require(manifest["opAddressing"]["kind"] == "stable-node-id");
    require(manifest["opAddressing"]["nodeIdJsonType"] == "decimal-string");
    require(manifest["opAddressing"]["snapshotNodeIdJsonType"] == "decimal-string");
    require(manifest == manifestAgain);
    require(manifest["properties"]["LightNode"].is_array());
    require(manifest["properties"]["Water"].is_array());
    require(manifest["properties"]["ParticleSystem"].is_array());
    require(manifest["sceneSnapshot"]["schema"] == saida::format::kSceneVersion);
    require(manifest["sceneSnapshot"]["version"] == saida::format::kSceneVersion);
    require(!manifest["sceneSnapshot"]["atomicLoad"].get<bool>());
    require(manifest["sceneSnapshot"]["failureState"] == "empty-scene");
    require(manifest["sceneSnapshot"]["unsupportedTypePolicy"] == "reject");

    // The formats block is the release manifest's single source of version truth.
    const json& formats = manifest["formats"];
    require(formats["opVersion"] == saida::authoring::kOpVersion);
    require(formats["scene"] == saida::format::kSceneVersion);
    require(formats["project"] == saida::format::kProjectVersion);
    require(formats["assetRegistry"] == saida::format::kAssetRegistryVersion);
    require(formats["scenario"] == saida::format::kScenarioVersion);
    require(formats["bootManifest"] == saida::format::kBootManifestVersion);
    for (const char* animFormat : {"rig", "clipView", "animGraph", "sequence",
                                   "retargetProfile"}) {
        require(formats[animFormat].is_number_integer());
        require(formats[animFormat].get<int>() > 0);
    }

    bool hasLightIntensity = false;
    for (const auto& prop : manifest["properties"]["LightNode"]) {
        if (prop["name"] == "intensity" && prop["kind"] == "float") {
            hasLightIntensity = true;
        }
    }
    require(hasLightIntensity);
}

void testManifestContainsBehavioursSignalsAndScenario() {
    json manifest = saida::authoring::buildEngineManifest();
    require(manifest["behaviours"].is_array());
    require(manifest["scenario"]["actions"].is_array());
    require(manifest["scenario"]["conditions"].is_array());

    bool hasRotatorSignal = false;
    bool hasHealthSlot = false;
    bool hasScenarioRunnerSignals = false;
    for (const auto& behaviour : manifest["behaviours"]) {
        if (behaviour["name"] == "Rotator") {
            for (const auto& signal : behaviour.value("signals", json::array()))
                if (signal["name"] == "fullRotation" && signal["arity"] == 0)
                    hasRotatorSignal = true;
        }
        if (behaviour["name"] == "Health") {
            for (const auto& slot : behaviour.value("slots", json::array()))
                if (slot["name"] == "damage")
                    hasHealthSlot = true;
        }
        if (behaviour["name"] == "ScenarioRunner") {
            for (const auto& signal : behaviour.value("signals", json::array()))
                if (signal["name"] == "finished")
                    hasScenarioRunnerSignals = true;
        }
    }
    require(hasRotatorSignal);
    require(hasHealthSlot);
    require(hasScenarioRunnerSignals);

    bool hasSignalEmit = false;
    for (const auto& action : manifest["scenario"]["actions"])
        if (action["name"] == "signal.emit") hasSignalEmit = true;
    require(hasSignalEmit);

    bool hasCompositeAll = false;
    bool hasTimelineFinished = false;
    for (const auto& condition : manifest["scenario"]["conditions"]) {
        if (condition["name"] == "all" && condition.value("composite", false))
            hasCompositeAll = true;
        if (condition["name"] == "timeline.finished")
            hasTimelineFinished = true;
    }
    require(hasCompositeAll);
    require(hasTimelineFinished);
}

void testSnapshotReflectsAppliedOps() {
    saida::Scene scene;
    auto light = std::make_unique<saida::LightNode>("SnapshotSun", saida::LightType::Directional);
    saida::Node* snapshotSun = scene.addChild(std::move(light));

    json res = applyOp(scene, setProperty(snapshotSun->id(), "intensity", 2.25f));
    require(res["ok"].get<bool>());

    json snapshot = json::parse(saida::authoring::serializeSceneSnapshot(scene, nullptr));
    require(snapshot["schema"].get<int>() == saida::format::kSceneVersion);
    require(snapshot["version"].get<int>() == saida::format::kSceneVersion);
    require(snapshot["snapshotMode"] == "authoring-headless");
    require(snapshot["scene"]["type"] == "Scene");
    require(snapshot["scene"]["children"].is_array());
    require(snapshot["scene"]["children"].size() == 1);

    const json& child = snapshot["scene"]["children"][0];
    require(child["type"] == "LightNode");
    require(child["name"] == "SnapshotSun");
    require(child["id"].is_string());
    require(child["id"] == nodeRef(snapshotSun->id()));
    require(close(child["intensity"].get<float>(), 2.25f));
    require(child["transform"]["position"].is_array());
}

// Phase B3: the headless snapshot deserializes back without a GPU and the
// round-trip (serialize -> deserialize -> serialize) is bit-stable. This is
// the foundation for the apply-ops tool and the editing/collaboration representation.
void testHeadlessSnapshotRoundTrip() {
    saida::Scene scene;
    scene.setName("Root");
    scene.assignSerializedId(1);

    auto light = std::make_unique<saida::LightNode>("Sun", saida::LightType::Directional);
    light->assignSerializedId(2);
    light->intensity = 2.25f;
    light->transform().position = {1.0f, 2.0f, 3.0f};
    light->addToGroup("lights");
    scene.addChild(std::move(light));

    auto* group = scene.createChild<saida::Node>("Group");
    group->assignSerializedId(3);
    group->addBehaviour(std::make_unique<saida::RotatorBehaviour>());
    auto script = std::make_unique<saida::ScriptBehaviour>();
    script->setScriptPath("scripts/contract.mjs");
    script->exportNumberProperty("speed", 3.5);
    group->addBehaviour(std::move(script));
    group->createChild<saida::ParticleSystemNode>()->assignSerializedId(4);

    auto* area = scene.createChild<saida::AreaNode>();
    area->assignSerializedId(5);
    area->setName("Trigger");
    area->friction = 0.25f;
    area->restitution = 0.75f;
    area->moving = true;

    const std::string s1 = saida::authoring::serializeSceneSnapshot(scene, nullptr);

    saida::Scene reloaded;
    std::string error;
    require(saida::authoring::deserializeSceneSnapshot(s1, reloaded, &error));
    require(error.empty());

    // Structure + ids + reflected props restored.
    require(reloaded.name() == "Root");
    require(reloaded.id() == 1);
    require(reloaded.children().size() == 3);
    saida::Node* sun = findChildByName(reloaded, "Sun");
    require(sun != nullptr);
    require(sun->id() == 2);
    require(sun->isInGroup("lights"));
    require(close(sun->transform().position.y, 2.0f));
    auto* sunLight = dynamic_cast<saida::LightNode*>(sun);
    require(sunLight != nullptr);
    require(close(sunLight->intensity, 2.25f));
    require(sunLight->type == saida::LightType::Directional);

    auto* trigger = dynamic_cast<saida::AreaNode*>(findChildByName(reloaded, "Trigger"));
    require(trigger != nullptr);
    require(close(trigger->friction, 0.25f));
    require(close(trigger->restitution, 0.75f));
    require(trigger->moving);

    // Bit-stable round-trip.
    const std::string s2 = saida::authoring::serializeSceneSnapshot(reloaded, nullptr);
    require(s1 == s2);

    // An op applies to the reloaded scene (load -> apply -> save chain).
    json res = applyOp(reloaded, setProperty(sun->id(), "intensity", 7.0f));
    require(res["ok"].get<bool>());
    require(close(sunLight->intensity, 7.0f));

    // Malformed document: clean failure, scene left empty.
    saida::Scene bad;
    require(!saida::authoring::deserializeSceneSnapshot(std::string("{\"nope\":1}"), bad, &error));
    require(!error.empty());
    require(bad.children().empty());
    require(!saida::authoring::deserializeSceneSnapshot(std::string("not json"), bad, &error));
}

void testHeadlessSnapshotFailsClosed() {
    saida::Scene source;
    source.setName("Root");
    source.createChild<saida::LightNode>()->setName("Known");
    const json valid = json::parse(saida::authoring::serializeSceneSnapshot(source, nullptr));

    // A misspelled node type must never be downgraded to Node.
    json unknownNode = valid;
    unknownNode["scene"]["children"].push_back({
        {"type", "MissingNode"},
        {"id", "999"},
        {"name", "Unsupported"},
        {"enabled", true},
        {"transform", {{"position", {0, 0, 0}},
                       {"rotation", {0, 0, 0, 1}},
                       {"scale", {1, 1, 1}}}},
        {"behaviours", json::array()},
        {"children", json::array()},
    });
    saida::Scene out;
    std::string error;
    require(!saida::authoring::deserializeSceneSnapshot(unknownNode, out, &error));
    require(error.find("scene.children[1]") != std::string::npos);
    require(error.find("unsupported node type 'MissingNode'") != std::string::npos);
    require(out.children().empty());

    // Unknown behaviours are equally destructive if ignored, so reject them.
    json unknownBehaviour = valid;
    unknownBehaviour["scene"]["children"][0]["behaviours"] = json::array({
        {{"type", "MissingBehaviour"}, {"enabled", true}}
    });
    require(!saida::authoring::deserializeSceneSnapshot(unknownBehaviour, out, &error));
    require(error.find("behaviours[0]") != std::string::npos);
    require(error.find("unsupported behaviour type") != std::string::npos);
    require(out.children().empty());

    json missingSchema = valid;
    missingSchema.erase("schema");
    require(!saida::authoring::deserializeSceneSnapshot(missingSchema, out, &error));
    require(error.find("schema is required") != std::string::npos);

    json numericId = valid;
    numericId["scene"]["id"] = 42;
    require(!saida::authoring::deserializeSceneSnapshot(numericId, out, &error));
    require(error.find("decimal string") != std::string::npos);

    json mismatch = valid;
    mismatch["version"] = saida::format::kSceneVersion - 1;
    require(!saida::authoring::deserializeSceneSnapshot(mismatch, out, &error));
    require(error.find("schema/version mismatch") != std::string::npos);

    json future = valid;
    future["schema"] = saida::format::kSceneVersion + 1;
    future["version"] = saida::format::kSceneVersion + 1;
    require(!saida::authoring::deserializeSceneSnapshot(future, out, &error));
    require(error.find("unsupported snapshot schema") != std::string::npos);

    // Camera is part of the shared durable contract (native headless + web).
    saida::Scene cameraSource;
    auto* camera = cameraSource.createChild<saida::CameraNode>();
    camera->setName("AuthoringCamera");
    camera->fovDegrees = 72.0f;
    camera->nearZ = 0.25f;
    camera->farZ = 900.0f;
    camera->priority = 4;
    camera->active = false;
    const json cameraDoc = json::parse(
        saida::authoring::serializeSceneSnapshot(cameraSource, nullptr));
    saida::Scene cameraReloaded;
    require(saida::authoring::deserializeSceneSnapshot(cameraDoc, cameraReloaded, &error));
    auto* reloadedCamera = dynamic_cast<saida::CameraNode*>(cameraReloaded.children()[0].get());
    require(reloadedCamera != nullptr);
    require(close(reloadedCamera->fovDegrees, 72.0f));
    require(close(reloadedCamera->nearZ, 0.25f));
    require(close(reloadedCamera->farZ, 900.0f));
    require(reloadedCamera->priority == 4);
    require(!reloadedCamera->active);

    // Fail before emitting a lossy document when a live scene contains a type
    // outside the headless contract.
    struct UnsupportedNode final : saida::Node {
        UnsupportedNode() : Node("Unsupported") {}
        const char* typeName() const override { return "UnsupportedNode"; }
    };
    saida::Scene unsupportedSource;
    unsupportedSource.createChild<UnsupportedNode>();
    bool serializationRejected = false;
    try {
        (void)saida::authoring::serializeSceneSnapshot(unsupportedSource, nullptr);
    } catch (const std::exception& e) {
        serializationRejected = std::string(e.what()).find("scene.children[0]") !=
                                std::string::npos;
    }
    require(serializationRejected);
}

// Track 1-F: the headless snapshot carries a MeshNode's mesh/material refs
// (mesh, texture, PBR params, lods) without resolving them -- no more render
// loss across the deserialize -> apply -> serialize fold.
void testMeshResourceRefsRoundTrip() {
    // Source document: a "rich" MeshNode as the full format would produce
    // (numeric AssetID refs + string path + material params).
    json doc = {
        {"schema", saida::format::kSceneVersion},
        {"version", saida::format::kSceneVersion},
        {"snapshotMode", "authoring-headless"},
        {"scene", {
            {"type", "Scene"}, {"id", "1"}, {"name", "Root"}, {"enabled", true},
            {"transform", {{"position", {0.0f, 0.0f, 0.0f}},
                           {"rotation", {0.0f, 0.0f, 0.0f, 1.0f}},
                           {"scale", {1.0f, 1.0f, 1.0f}}}},
            {"behaviours", json::array()},
            {"children", json::array({json{
                {"type", "MeshNode"}, {"id", "2"}, {"name", "Palm_A"}, {"enabled", true},
                {"transform", {{"position", {1.0f, 2.0f, 3.0f}},
                               {"rotation", {0.0f, 0.0f, 0.0f, 1.0f}},
                               {"scale", {1.0f, 1.0f, 1.0f}}}},
                {"behaviours", json::array()},
                {"children", json::array()},
                {"mesh", "models/palm_a.obj"},
                {"texture", 42},
                {"baseColor", {1.0f, 0.9f, 0.8f, 1.0f}},
                {"metallic", 0.1f},
                {"roughness", 0.7f},
                {"ao", 1.0f},
                {"emissive", {0.0f, 0.0f, 0.0f, 0.0f}},
                {"shader", "lit"},
                {"castShadows", true},
                {"meshEnabled", true},
                {"outlineEnabled", false},
                {"outlineColor", {0.02f, 0.02f, 0.02f, 1.0f}},
                {"outlineWidth", 3.0f},
            }})},
        }},
    };

    saida::Scene scene;
    std::string error;
    require(saida::authoring::deserializeSceneSnapshot(doc, scene, &error));
    require(error.empty());

    // The refs are captured on the node (data only, no GPU resource).
    auto* palm = dynamic_cast<saida::MeshNode*>(findChildByName(scene, "Palm_A"));
    require(palm != nullptr);
    require(palm->mesh() == nullptr);
    require(!palm->durableResourceRefs().empty());

    // They come back out identical in the headless serialization.
    json out = json::parse(saida::authoring::serializeSceneSnapshot(scene, nullptr));
    const json& child = out["scene"]["children"][0];
    require(child["mesh"] == "models/palm_a.obj");
    require(child["texture"] == 42);
    require(child["shader"] == "lit");
    require(close(child["roughness"].get<float>(), 0.7f));
    require(child["baseColor"].is_array() && child["baseColor"].size() == 4);
    require(child["castShadows"] == true);

    // They survive an op (the apply-ops fold path).
    json res = applyOp(scene, json{{"type", "set_transform"},
                                   {"payload", {{"nodeId", nodeRef(palm->id())},
                                                {"position", {9.0f, 0.0f, 0.0f}}}}});
    require(res["ok"].get<bool>());
    json afterOp = json::parse(saida::authoring::serializeSceneSnapshot(scene, nullptr));
    require(afterOp["scene"]["children"][0]["mesh"] == "models/palm_a.obj");
    require(close(afterOp["scene"]["children"][0]["transform"]["position"][0].get<float>(), 9.0f));

    // Bit-stable round-trip (serialize -> deserialize -> serialize).
    const std::string s1 = saida::authoring::serializeSceneSnapshot(scene, nullptr);
    saida::Scene reloaded;
    require(saida::authoring::deserializeSceneSnapshot(s1, reloaded, &error));
    const std::string s2 = saida::authoring::serializeSceneSnapshot(reloaded, nullptr);
    require(s1 == s2);

    // A MeshNode with no refs (e.g. created by create_node) stays without
    // refs: the flags are emitted, no mesh/texture key is invented.
    saida::Scene fresh;
    fresh.createChild<saida::MeshNode>()->setName("Bare");
    json bare = json::parse(saida::authoring::serializeSceneSnapshot(fresh, nullptr));
    const json& bareChild = bare["scene"]["children"][0];
    require(bareChild.contains("castShadows"));
    require(!bareChild.contains("mesh"));
    require(!bareChild.contains("texture"));
}

// set_scene_setting: edits SceneSettings by name (float/bool/vec3), invertible.
void testSceneSettingOp() {
    saida::Scene scene;
    auto sceneSetting = [&](const std::string& key, json value) {
        return json{{"type", "set_scene_setting"}, {"payload", {{"setting", key}, {"value", std::move(value)}}}};
    };

    // float
    json res = applyOp(scene, sceneSetting("fogDensity", 0.08f));
    require(res["ok"].get<bool>());
    require(close(scene.settings().fogDensity, 0.08f));
    require(res.contains("inverse"));

    // bool
    const bool bloomBefore = scene.settings().bloomEnabled;
    res = applyOp(scene, sceneSetting("bloomEnabled", !bloomBefore));
    require(res["ok"].get<bool>());
    require(scene.settings().bloomEnabled == !bloomBefore);

    // vec3 colour into vec4 field (alpha preserved)
    scene.settings().ambientLight = {0.1f, 0.2f, 0.3f, 1.0f};
    res = applyOp(scene, sceneSetting("ambientLight", json::array({0.5f, 0.6f, 0.7f})));
    require(res["ok"].get<bool>());
    require(close(scene.settings().ambientLight.x, 0.5f));
    require(close(scene.settings().ambientLight.z, 0.7f));
    require(close(scene.settings().ambientLight.w, 1.0f));  // alpha untouched

    // inverse restores the previous colour
    applyOp(scene, res["inverse"]);
    require(close(scene.settings().ambientLight.x, 0.1f));
    require(close(scene.settings().ambientLight.z, 0.3f));

    // rejections: unknown setting, wrong type
    res = applyOp(scene, sceneSetting("doesNotExist", 1.0f));
    require(!res["ok"].get<bool>());
    res = applyOp(scene, sceneSetting("fogDensity", true));
    require(!res["ok"].get<bool>());
    res = applyOp(scene, sceneSetting("bloomEnabled", 3.0f));
    require(!res["ok"].get<bool>());
}

// Behaviour authoring ops: attach gameplay logic, set reflected props, remove;
// behaviours round-trip through the headless snapshot.
void testBehaviourOps() {
    saida::Scene scene;
    auto* n = scene.createChild<saida::Node>("Spinner");
    n->assignSerializedId(5);

    auto behaviourOp = [](const std::string& type, json payload) {
        return json{{"type", type}, {"payload", std::move(payload)}};
    };

    // add_behaviour → inverse is remove_behaviour
    json res = applyOp(scene, behaviourOp("add_behaviour",
                                          {{"nodeId", nodeRef(n->id())}, {"behaviourType", "Rotator"}}));
    require(res["ok"].get<bool>());
    require(res["inverse"]["type"] == "remove_behaviour");

    // duplicate of the same type rejected
    res = applyOp(scene, behaviourOp("add_behaviour",
                                     {{"nodeId", nodeRef(n->id())}, {"behaviourType", "Rotator"}}));
    require(!res["ok"].get<bool>());

    // unknown behaviour type rejected
    res = applyOp(scene, behaviourOp("add_behaviour",
                                     {{"nodeId", nodeRef(n->id())}, {"behaviourType", "Nonesuch"}}));
    require(!res["ok"].get<bool>());

    // set_behaviour_property (Rotator.speed is a float)
    res = applyOp(scene, behaviourOp("set_behaviour_property",
                                     {{"nodeId", nodeRef(n->id())}, {"behaviourType", "Rotator"},
                                      {"property", "speed"}, {"value", 90.0f}}));
    require(res["ok"].get<bool>());
    require(close(res["diff"]["after"].get<float>(), 90.0f));
    require(res["inverse"]["type"] == "set_behaviour_property");

    // unknown behaviour property rejected
    res = applyOp(scene, behaviourOp("set_behaviour_property",
                                     {{"nodeId", nodeRef(n->id())}, {"behaviourType", "Rotator"},
                                      {"property", "wobble"}, {"value", 1.0f}}));
    require(!res["ok"].get<bool>());

    // headless snapshot carries the behaviour + its property
    json snap = json::parse(saida::authoring::serializeSceneSnapshot(scene, nullptr));
    const json& child = snap["scene"]["children"][0];
    require(child["behaviours"].size() == 1);
    require(child["behaviours"][0]["type"] == "Rotator");
    require(close(child["behaviours"][0]["speed"].get<float>(), 90.0f));

    // round-trip: deserialize + re-serialize is stable at the byte level
    saida::Scene reloaded;
    std::string err2;
    require(saida::authoring::deserializeSceneSnapshot(snap, reloaded, &err2));
    json snap2 = json::parse(saida::authoring::serializeSceneSnapshot(reloaded, nullptr));
    require(snap == snap2);

    // remove_behaviour (marked non-op-invertible)
    res = applyOp(scene, behaviourOp("remove_behaviour",
                                     {{"nodeId", nodeRef(n->id())}, {"behaviourType", "Rotator"}}));
    require(res["ok"].get<bool>());
    require(res["diff"]["invertible"].get<bool>() == false);
    snap = json::parse(saida::authoring::serializeSceneSnapshot(scene, nullptr));
    require(snap["scene"]["children"][0]["behaviours"].empty());
}

void testSignalConnectionOps() {
    saida::Scene scene;
    saida::Node* a = scene.addChild(
        std::make_unique<saida::LightNode>("A", saida::LightType::Point));
    saida::Node* b = scene.addChild(
        std::make_unique<saida::LightNode>("B", saida::LightType::Point));

    auto connOp = [a, b](const std::string& type) {
        return json{{"type", type},
                    {"payload", {{"fromNodeId", nodeRef(a->id())}, {"signal", "died"},
                                 {"toNodeId", nodeRef(b->id())}, {"slot", "onDied"}}}};
    };

    // add: succeeds, is invertible to remove_signal_connection
    json res = applyOp(scene, connOp("add_signal_connection"));
    require(res["ok"].get<bool>());
    require(res["inverse"]["type"] == "remove_signal_connection");
    require(scene.connections().size() == 1);

    // duplicate add is rejected without mutating
    res = applyOp(scene, connOp("add_signal_connection"));
    require(!res["ok"].get<bool>());
    require(scene.connections().size() == 1);

    // unknown node is rejected
    res = applyOp(scene, json{{"type", "add_signal_connection"},
                              {"payload", {{"fromNodeId", nodeRef(a->id())}, {"signal", "died"},
                                           {"toNodeId", "999999"}, {"slot", "onDied"}}}});
    require(!res["ok"].get<bool>());

    // the connection round-trips through the headless snapshot
    json snap = json::parse(saida::authoring::serializeSceneSnapshot(scene, nullptr));
    require(snap["scene"].contains("connections"));
    require(snap["scene"]["connections"].size() == 1);
    require(snap["scene"]["connections"][0]["from"] == nodeRef(a->id()));
    require(snap["scene"]["connections"][0]["to"] == nodeRef(b->id()));
    saida::Scene reloaded;
    std::string err;
    require(saida::authoring::deserializeSceneSnapshot(snap, reloaded, &err));
    json snap2 = json::parse(saida::authoring::serializeSceneSnapshot(reloaded, nullptr));
    require(snap == snap2);

    // remove: succeeds, is invertible to add_signal_connection
    res = applyOp(scene, connOp("remove_signal_connection"));
    require(res["ok"].get<bool>());
    require(res["inverse"]["type"] == "add_signal_connection");
    require(scene.connections().empty());

    // removing a missing connection is rejected
    res = applyOp(scene, connOp("remove_signal_connection"));
    require(!res["ok"].get<bool>());
}

void testRuntimeSceneTypeContractFailsClosed() {
    saida::NodeRegistry::instance().registerType<saida::Node>("Node");
    saida::BehaviourRegistry::instance().registerType<saida::RotatorBehaviour>("Rotator");

    std::string error;
    const json supported = {
        {"type", "Node"},
        {"children", json::array({
            json{{"type", "Node"},
                 {"behaviours", json::array({json{{"type", "Rotator"}}})}},
        })},
    };
    require(saida::SceneSerializer::validateTypeContractJson(supported.dump(), &error));

    json unsupportedNode = supported;
    unsupportedNode["children"][0]["type"] = "DesktopOnlyNode";
    require(!saida::SceneSerializer::validateTypeContractJson(unsupportedNode.dump(), &error));
    require(error.find("unsupported node type 'DesktopOnlyNode'") != std::string::npos);

    json unsupportedBehaviour = supported;
    unsupportedBehaviour["children"][0]["behaviours"][0]["type"] = "EditorOnlyBehaviour";
    require(!saida::SceneSerializer::validateTypeContractJson(
        unsupportedBehaviour.dump(), &error));
    require(error.find("unsupported behaviour type 'EditorOnlyBehaviour'") !=
            std::string::npos);

    json malformed = supported;
    malformed["children"][0]["type"] = 42;
    require(!saida::SceneSerializer::validateTypeContractJson(malformed.dump(), &error));
    require(error.find("type must be a string") != std::string::npos);
}

} // namespace

int main() {
    testReflectedLightProperty();
    testReflectedWaterAndParticleProperties();
    testInvalidOpsDoNotMutate();
    testCreateNodeResourceBoundaries();
    testSaidaOpRoundTrip();
    testSaidaOpParseRejections();
    testReparentNode();
    testInverseOps();
    testStableNodeIdAddressing();
    testValidateOpShape();
    testManifestListsRegistryOps();
    testManifestContainsReflectedProperties();
    testManifestContainsBehavioursSignalsAndScenario();
    testSnapshotReflectsAppliedOps();
    testHeadlessSnapshotRoundTrip();
    testHeadlessSnapshotFailsClosed();
    testMeshResourceRefsRoundTrip();
    testSceneSettingOp();
    testBehaviourOps();
    testSignalConnectionOps();
    testRuntimeSceneTypeContractFailsClosed();
    return 0;
}
