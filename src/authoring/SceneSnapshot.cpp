#include "authoring/SceneSnapshot.hpp"

#include "core/FormatVersions.hpp"
#include "core/Reflection.hpp"
#include "graphics/ResourceManager.hpp"
#include "scene/Behaviour.hpp"
#include "scene/BehaviourRegistry.hpp"
#include "nodes/CameraNode.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/MeshNode.hpp"
#include "scene/Node.hpp"
#include "scene/NodeRegistry.hpp"
#include "nodes/ParticleSystemNode.hpp"
#include "scene/ReflectedTypes.hpp"
#include "scene/RuntimeTypeMatrix.hpp"
#include "scene/Scene.hpp"
#include "scene/SerializationHelpers.hpp"
#include "nodes/UICanvasNode.hpp"
#include "nodes/UINode.hpp"
#include "nodes/UITextNode.hpp"
#include "nodes/WaterNode.hpp"

#ifndef __EMSCRIPTEN__
#include "physics/AreaNode.hpp"
#include "physics/CharacterBodyNode.hpp"
#include "physics/CollisionObjectNode.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "physics/RigidBodyNode.hpp"
#include "physics/StaticBodyNode.hpp"
#endif

#include <nlohmann/json.hpp>

#include <glm/gtc/quaternion.hpp>

#include <charconv>
#include <exception>
#include <memory>
#include <stdexcept>

namespace saida::authoring {
namespace {

using json = nlohmann::json;

json transformToJson(const Transform& t) {
    return {
        {"position", vec3ToJson(t.position)},
        {"rotation", json::array({t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w})},
        {"scale", vec3ToJson(t.scale)},
    };
}

bool snapshotNodeIdFromJson(const json& value, NodeId& out) {
    if (!value.is_string()) return false;
    const std::string text = value.get<std::string>();
    NodeId parsed = kNodeInvalid;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (text.empty() || result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() || parsed == kNodeInvalid)
        return false;
    out = parsed;
    return true;
}

void ensureHeadlessRegistries() {
    registerReflectedTypes();

    auto& nodes = NodeRegistry::instance();
    if (nodes.factories().find("Node") == nodes.factories().end())
        nodes.registerType<Node>("Node");
    if (nodes.factories().find("MeshNode") == nodes.factories().end())
        nodes.registerType<MeshNode>("MeshNode");
    if (nodes.factories().find("Camera") == nodes.factories().end())
        nodes.registerType<CameraNode>("Camera");
    if (nodes.factories().find("UINode") == nodes.factories().end())
        nodes.registerType<UINode>("UINode");
    if (nodes.factories().find("UICanvasNode") == nodes.factories().end())
        nodes.registerType<UICanvasNode>("UICanvasNode");
    if (nodes.factories().find("UITextNode") == nodes.factories().end())
        nodes.registerType<UITextNode>("UITextNode");

#ifndef __EMSCRIPTEN__
    if (nodes.factories().find("CollisionShape") == nodes.factories().end())
        nodes.registerType<CollisionShapeNode>("CollisionShape");
    if (nodes.factories().find("StaticBody") == nodes.factories().end())
        nodes.registerType<StaticBodyNode>("StaticBody");
    if (nodes.factories().find("RigidBody") == nodes.factories().end())
        nodes.registerType<RigidBodyNode>("RigidBody");
    if (nodes.factories().find("CharacterBody") == nodes.factories().end())
        nodes.registerType<CharacterBodyNode>("CharacterBody");
#endif

    std::string typeMatrixError;
#ifdef __EMSCRIPTEN__
    constexpr RuntimeTypeTarget matrixTarget = RuntimeTypeTarget::AuthoringWasm;
    constexpr bool allowAdditional = false;
#else
    // The desktop editor may call the headless codec after registering its
    // larger native catalog in the same process. Dedicated headless tests use
    // strict mode to reject extras as well as omissions.
    constexpr RuntimeTypeTarget matrixTarget = RuntimeTypeTarget::Headless;
    constexpr bool allowAdditional = true;
#endif
    if (!verifyRegisteredRuntimeTypes(matrixTarget, typeMatrixError, allowAdditional)) {
        throw std::runtime_error("runtime type matrix mismatch: " + typeMatrixError);
    }
}

const reflect::TypeDesc* reflectedNodeDesc(const Node& node) {
    const char* typeName = node.typeName();
    if (!typeName) return nullptr;
    const reflect::TypeDesc* desc = reflect::TypeRegistry::instance().find(typeName);
    return desc && desc->category == "node" ? desc : nullptr;
}

bool isSupportedHeadlessNodeType(const std::string& type) {
    if (type == "Node" || type == "MeshNode" || type == "Camera" ||
        type == "UINode" || type == "UICanvasNode" || type == "UITextNode")
        return true;
#ifndef __EMSCRIPTEN__
    if (type == "CollisionShape" || type == "StaticBody" ||
        type == "RigidBody" || type == "CharacterBody")
        return true;
#endif
    const reflect::TypeDesc* desc = reflect::TypeRegistry::instance().find(type);
    return desc && desc->category == "node";
}

bool isSupportedHeadlessBehaviourType(const std::string& type) {
    const reflect::TypeDesc* desc = reflect::TypeRegistry::instance().find(type);
    return desc && desc->category == "behaviour";
}

json serializeNodeWithoutResources(const Node& node, const std::string& path,
                                   bool sceneRoot = false) {
    ensureHeadlessRegistries();
    const std::string type = node.typeName() ? node.typeName() : "";
    if ((sceneRoot && type != "Scene") ||
        (!sceneRoot && !isSupportedHeadlessNodeType(type))) {
        throw std::runtime_error(path + ": unsupported node type '" + type +
                                 "' in headless snapshot");
    }

    json out;
    out["type"] = type;
    out["id"] = std::to_string(node.id());
    out["name"] = node.name();
    out["enabled"] = node.enabled();
    out["transform"] = transformToJson(node.transform());

    // Behaviours (gameplay logic) — same {type, enabled, ...reflected props}
    // shape as the full serializer, so the headless snapshot carries logic too.
    json behavioursJson = json::array();
    for (std::size_t i = 0; i < node.behaviours().size(); ++i) {
        const auto& b = node.behaviours()[i];
        const std::string behaviourPath = path + ".behaviours[" + std::to_string(i) + "]";
        const std::string behaviourType = b->typeName() ? b->typeName() : "";
        if (!isSupportedHeadlessBehaviourType(behaviourType)) {
            throw std::runtime_error(behaviourPath + ": unsupported behaviour type '" +
                                     behaviourType + "' in headless snapshot");
        }
        json bj;
        bj["type"] = behaviourType;
        bj["enabled"] = b->enabled();
        b->save(bj);
        behavioursJson.push_back(std::move(bj));
    }
    out["behaviours"] = std::move(behavioursJson);

    if (!node.importedFromPath().empty())
        out["importedFrom"] = node.importedFromPath();
    if (!node.groups().empty())
        out["groups"] = node.groups();

    if (const reflect::TypeDesc* desc = reflectedNodeDesc(node))
        desc->saveTo(&node, out);

#ifndef __EMSCRIPTEN__
    if (const auto* body = dynamic_cast<const CollisionObjectNode*>(&node)) {
        out["friction"] = body->friction;
        out["restitution"] = body->restitution;
    }
    if (const auto* area = dynamic_cast<const AreaNode*>(&node)) {
        out["moving"] = area->moving;
    }
    if (const auto* shape = dynamic_cast<const CollisionShapeNode*>(&node)) {
        out["shapeType"] = static_cast<int>(shape->shapeType);
        out["halfExtents"] = vec3ToJson(shape->halfExtents);
        out["radius"] = shape->radius;
        out["height"] = shape->height;
        out["axis"] = shape->axis;
        out["offset"] = vec3ToJson(shape->offset);
    }
    if (const auto* rigid = dynamic_cast<const RigidBodyNode*>(&node)) {
        out["kinematic"] = rigid->kinematic;
        out["mass"] = rigid->mass;
        out["gravityFactor"] = rigid->gravityFactor;
        out["linearDamping"] = rigid->linearDamping;
        out["angularDamping"] = rigid->angularDamping;
    }
    if (const auto* character = dynamic_cast<const CharacterBodyNode*>(&node)) {
        out["mass"] = character->mass;
        out["maxSlopeAngle"] = character->maxSlopeAngle;
    }
#endif

    // MeshNode (Track 1-F): the rendering-identity fields from the full format.
    // The flags come from the node; the resource refs (mesh, texture, PBR
    // params, lods) come from the durable blob captured at deserialization —
    // a headless scene can't query a ResourceManager, but it must no longer
    // lose this data across the round trip.
    if (std::string(node.typeName() ? node.typeName() : "") == "MeshNode") {
        const auto& mesh = static_cast<const MeshNode&>(node);
        out["castShadows"] = mesh.castShadows();
        out["meshEnabled"] = mesh.meshEnabled();
        out["outlineEnabled"] = mesh.outlineEnabled();
        const glm::vec4& oc = mesh.outlineColor();
        out["outlineColor"] = json::array({oc.r, oc.g, oc.b, oc.a});
        out["outlineWidth"] = mesh.outlineWidth();
        if (!mesh.durableResourceRefs().empty()) {
            json refs = json::parse(mesh.durableResourceRefs(), nullptr,
                                    /*allow_exceptions=*/false);
            if (refs.is_object())
                for (auto it = refs.begin(); it != refs.end(); ++it)
                    out[it.key()] = it.value();
        }
    }

    if (std::string(node.typeName() ? node.typeName() : "") == "Camera") {
        const auto& camera = static_cast<const CameraNode&>(node);
        out["fovDegrees"] = camera.fovDegrees;
        out["nearZ"] = camera.nearZ;
        out["farZ"] = camera.farZ;
        out["priority"] = camera.priority;
        out["active"] = camera.active;
    }

    if (const auto* ui = dynamic_cast<const UINode*>(&node)) {
        out["x"] = ui->x();
        out["y"] = ui->y();
        out["width"] = ui->width();
        out["height"] = ui->height();
        out["anchorX"] = ui->anchorX();
        out["anchorY"] = ui->anchorY();
        out["pivotX"] = ui->pivotX();
        out["pivotY"] = ui->pivotY();
    }
    if (const auto* canvas = dynamic_cast<const UICanvasNode*>(&node)) {
        out["width"] = canvas->width();
        out["height"] = canvas->height();
    }
    if (const auto* text = dynamic_cast<const UITextNode*>(&node)) {
        out["text"] = text->text();
        out["fontSize"] = text->fontSize();
        const glm::vec4 color = text->color();
        out["color"] = json::array({color.r, color.g, color.b, color.a});
    }

    json children = json::array();
    for (std::size_t i = 0; i < node.children().size(); ++i) {
        children.push_back(serializeNodeWithoutResources(
            *node.children()[i], path + ".children[" + std::to_string(i) + "]"));
    }
    out["children"] = std::move(children);
    return out;
}

json sceneSettingsToJson(const Scene& scene) {
    const SceneSettings& s = scene.settings();
    return {
        {"ambient", vec3ToJson(s.ambientLight)},
        {"clearColor", vec3ToJson(s.clearColor)},
        {"postProcessing", s.enablePostProcessing},
        {"lightingMode", static_cast<int>(s.lightingMode)},
        {"giEnabled", s.giEnabled},
        {"giMode", static_cast<int>(s.giMode)},
        {"giIntensity", s.giIntensity},
        {"skyboxTexture", s.skyboxTexture},
        {"skyboxExposure", s.skyboxExposure},
        {"skyboxRotation", s.skyboxRotation},
        {"iblEnabled", s.iblEnabled},
        {"iblDiffuseIntensity", s.iblDiffuseIntensity},
        {"iblSpecularIntensity", s.iblSpecularIntensity},
        {"aoEnabled", s.aoEnabled},
        {"aoRadius", s.aoRadius},
        {"aoIntensity", s.aoIntensity},
        {"aoPower", s.aoPower},
        {"fogEnabled", s.fogEnabled},
        {"fogColor", vec3ToJson(glm::vec3(s.fogColor))},
        {"fogStart", s.fogStart},
        {"fogDensity", s.fogDensity},
        {"bloomEnabled", s.bloomEnabled},
        {"bloomThreshold", s.bloomThreshold},
        {"bloomIntensity", s.bloomIntensity},
        {"bloomRadius", s.bloomRadius},
        {"changeRenderingAtLoad", s.changeRenderingAtLoad},
    };
}

json serializeSceneWithoutResources(Scene& scene) {
    json root = serializeNodeWithoutResources(scene, "scene", true);
    root["settings"] = sceneSettingsToJson(scene);
    if (!scene.connections().empty()) {
        json conns = json::array();
        for (const auto& c : scene.connections())
            conns.push_back({{"from", std::to_string(c.from)}, {"signal", c.signal},
                             {"to", std::to_string(c.to)}, {"slot", c.slot}});
        root["connections"] = std::move(conns);
    }
    return root;
}

// --- headless deserialization (inverse of serializeSceneWithoutResources) ----

// Build only types covered by the headless contract. A type that exists in a
// desktop-only registry is not automatically safe here: accepting it without a
// resource-free codec would silently discard its properties.
std::unique_ptr<Node> makeHeadlessNode(const std::string& type,
                                       const std::string& path,
                                       std::string& error) {
    ensureHeadlessRegistries();
    if (!isSupportedHeadlessNodeType(type)) {
        error = path + ": unsupported node type '" + type + "' in headless snapshot";
        return nullptr;
    }
    std::unique_ptr<Node> node = NodeRegistry::instance().create(type);
    if (!node) {
        error = path + ": node type '" + type + "' has no headless factory";
        return nullptr;
    }
    return node;
}

// Restore the resource-free fields written by serializeNodeWithoutResources.
// Children are handled by the caller so the root Scene can reuse this too.
bool loadNodeFields(Node& node, const json& j, const std::string& path,
                    std::string& error) {
    if (auto it = j.find("id"); it != j.end()) {
        NodeId id = kNodeInvalid;
        if (!snapshotNodeIdFromJson(*it, id)) {
            error = path + ".id: expected a non-zero decimal string";
            return false;
        }
        node.assignSerializedId(id);
    }
    node.setName(j.value("name", node.name()));
    node.setEnabled(j.value("enabled", true));

    if (auto it = j.find("transform"); it != j.end() && it->is_object()) {
        Transform& t = node.transform();
        t.position = jsonToVec3(it->value("position", json()), t.position);
        if (auto r = it->find("rotation"); r != it->end() && r->is_array() && r->size() == 4)
            t.rotation = glm::quat((*r)[3].get<float>(), (*r)[0].get<float>(),
                                   (*r)[1].get<float>(), (*r)[2].get<float>());
        t.scale = jsonToVec3(it->value("scale", json()), t.scale);
    }

    if (auto it = j.find("groups"); it != j.end() && it->is_array())
        for (const auto& g : *it)
            if (g.is_string()) node.addToGroup(g.get<std::string>());

    if (auto it = j.find("importedFrom"); it != j.end() && it->is_string())
        node.setImportedFromPath(it->get<std::string>());

    if (const reflect::TypeDesc* desc = reflectedNodeDesc(node))
        desc->loadFrom(&node, j);

#ifndef __EMSCRIPTEN__
    if (auto* body = dynamic_cast<CollisionObjectNode*>(&node)) {
        if (auto it = j.find("friction"); it != j.end()) body->friction = it->get<float>();
        if (auto it = j.find("restitution"); it != j.end()) body->restitution = it->get<float>();
    }
    if (auto* area = dynamic_cast<AreaNode*>(&node)) {
        if (auto it = j.find("moving"); it != j.end()) area->moving = it->get<bool>();
    }
    if (auto* shape = dynamic_cast<CollisionShapeNode*>(&node)) {
        if (auto it = j.find("shapeType"); it != j.end())
            shape->shapeType = static_cast<CollisionShapeType>(it->get<int>());
        shape->halfExtents = jsonToVec3(j.value("halfExtents", json()), shape->halfExtents);
        if (auto it = j.find("radius"); it != j.end()) shape->radius = it->get<float>();
        if (auto it = j.find("height"); it != j.end()) shape->height = it->get<float>();
        if (auto it = j.find("axis"); it != j.end()) shape->axis = it->get<int>();
        shape->offset = jsonToVec3(j.value("offset", json()), shape->offset);
    }
    if (auto* rigid = dynamic_cast<RigidBodyNode*>(&node)) {
        if (auto it = j.find("kinematic"); it != j.end()) rigid->kinematic = it->get<bool>();
        if (auto it = j.find("mass"); it != j.end()) rigid->mass = it->get<float>();
        if (auto it = j.find("gravityFactor"); it != j.end())
            rigid->gravityFactor = it->get<float>();
        if (auto it = j.find("linearDamping"); it != j.end())
            rigid->linearDamping = it->get<float>();
        if (auto it = j.find("angularDamping"); it != j.end())
            rigid->angularDamping = it->get<float>();
    }
    if (auto* character = dynamic_cast<CharacterBodyNode*>(&node)) {
        if (auto it = j.find("mass"); it != j.end()) character->mass = it->get<float>();
        if (auto it = j.find("maxSlopeAngle"); it != j.end())
            character->maxSlopeAngle = it->get<float>();
    }
#endif

    // MeshNode (Track 1-F): restores the rendering flags and captures the
    // resource refs as-is (resolved later by a runtime that has a
    // ResourceManager; re-serialized identically by the headless path).
    if (std::string(node.typeName() ? node.typeName() : "") == "MeshNode") {
        auto& mesh = static_cast<MeshNode&>(node);
        if (auto c = j.find("castShadows"); c != j.end() && c->is_boolean())
            mesh.castShadows() = c->get<bool>();
        if (auto c = j.find("meshEnabled"); c != j.end() && c->is_boolean())
            mesh.setMeshEnabled(c->get<bool>());
        if (auto c = j.find("outlineEnabled"); c != j.end() && c->is_boolean())
            mesh.setOutlineEnabled(c->get<bool>());
        if (auto c = j.find("outlineColor"); c != j.end() && c->is_array() && c->size() == 4)
            mesh.outlineColor() = glm::vec4((*c)[0].get<float>(), (*c)[1].get<float>(),
                                            (*c)[2].get<float>(), (*c)[3].get<float>());
        if (auto c = j.find("outlineWidth"); c != j.end() && c->is_number())
            mesh.outlineWidth() = c->get<float>();
        mesh.captureDurableResourceRefs(j);
    }

    if (std::string(node.typeName() ? node.typeName() : "") == "Camera") {
        auto& camera = static_cast<CameraNode&>(node);
        if (auto it = j.find("fovDegrees"); it != j.end() && it->is_number())
            camera.fovDegrees = it->get<float>();
        if (auto it = j.find("nearZ"); it != j.end() && it->is_number())
            camera.nearZ = it->get<float>();
        if (auto it = j.find("farZ"); it != j.end() && it->is_number())
            camera.farZ = it->get<float>();
        if (auto it = j.find("priority"); it != j.end() && it->is_number_integer())
            camera.priority = it->get<int>();
        if (auto it = j.find("active"); it != j.end() && it->is_boolean())
            camera.active = it->get<bool>();
    }

    if (auto* ui = dynamic_cast<UINode*>(&node)) {
        ui->setPosition(j.value("x", 0.0f), j.value("y", 0.0f));
        ui->setSize(j.value("width", 100.0f), j.value("height", 100.0f));
        ui->setAnchor(j.value("anchorX", 0.5f), j.value("anchorY", 0.5f));
        ui->setPivot(j.value("pivotX", 0.5f), j.value("pivotY", 0.5f));
    }
    if (auto* canvas = dynamic_cast<UICanvasNode*>(&node)) {
        canvas->setSize(j.value("width", 1920.0f), j.value("height", 1080.0f));
    }
    if (auto* text = dynamic_cast<UITextNode*>(&node)) {
        text->setText(j.value("text", std::string("Text")));
        text->setFontSize(j.value("fontSize", 16.0f));
        if (auto color = j.find("color"); color != j.end() &&
            color->is_array() && color->size() == 4) {
            text->setColor(glm::vec4((*color)[0].get<float>(), (*color)[1].get<float>(),
                                     (*color)[2].get<float>(), (*color)[3].get<float>()));
        }
    }

    // Behaviours: rebuild by type from the registry, then load reflected props.
    if (auto it = j.find("behaviours"); it != j.end()) {
        if (!it->is_array()) {
            error = path + ".behaviours: expected an array";
            return false;
        }
        ensureHeadlessRegistries();
        for (std::size_t i = 0; i < it->size(); ++i) {
            const json& bj = (*it)[i];
            const std::string behaviourPath = path + ".behaviours[" + std::to_string(i) + "]";
            if (!bj.is_object() || !bj.contains("type") || !bj["type"].is_string()) {
                error = behaviourPath + ": behaviour needs a string 'type'";
                return false;
            }
            const std::string tn = bj["type"].get<std::string>();
            if (!isSupportedHeadlessBehaviourType(tn)) {
                error = behaviourPath + ": unsupported behaviour type '" + tn +
                        "' in headless snapshot";
                return false;
            }
            auto b = BehaviourRegistry::instance().create(tn);
            if (!b) {
                error = behaviourPath + ": behaviour type '" + tn +
                        "' has no headless factory";
                return false;
            }
            if (bj.contains("enabled") && bj["enabled"].is_boolean())
                b->setEnabled(bj["enabled"].get<bool>());
            b->load(bj);
            node.addBehaviour(std::move(b));
        }
    }
    return true;
}

std::unique_ptr<Node> deserializeNodeHeadless(const json& j, const std::string& path,
                                              std::string& error) {
    if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) {
        error = path + ": node needs a string 'type'";
        return nullptr;
    }
    const std::string type = j["type"].get<std::string>();
    std::unique_ptr<Node> node = makeHeadlessNode(type, path, error);
    if (!node || !loadNodeFields(*node, j, path, error)) return nullptr;
    if (auto it = j.find("children"); it != j.end()) {
        if (!it->is_array()) {
            error = path + ".children: expected an array";
            return nullptr;
        }
        for (std::size_t i = 0; i < it->size(); ++i) {
            const std::string childPath = path + ".children[" + std::to_string(i) + "]";
            std::unique_ptr<Node> child = deserializeNodeHeadless((*it)[i], childPath, error);
            if (!child) return nullptr;
            node->addChild(std::move(child));
        }
    }
    return node;
}

// Mirror of sceneSettingsToJson — reads exactly the keys it writes, so a
// snapshot round-trips (serialize -> deserialize -> serialize) unchanged.
void loadSceneSettings(Scene& scene, const json& s) {
    SceneSettings& out = scene.settings();
    out.ambientLight = glm::vec4(jsonToVec3(s.value("ambient", json())), 0.0f);
    out.clearColor = glm::vec4(jsonToVec3(s.value("clearColor", json())), 1.0f);
    out.enablePostProcessing = s.value("postProcessing", true);
    out.lightingMode = static_cast<LightingMode>(s.value("lightingMode", 0));
    out.giEnabled = s.value("giEnabled", true);
    out.giMode = static_cast<GIMode>(s.value("giMode", 0));
    out.giIntensity = s.value("giIntensity", 1.0f);
    out.skyboxTexture = s.value("skyboxTexture", kAssetInvalid);
    out.skyboxExposure = s.value("skyboxExposure", 1.0f);
    out.skyboxRotation = s.value("skyboxRotation", 0.0f);
    out.iblEnabled = s.value("iblEnabled", true);
    out.iblDiffuseIntensity = s.value("iblDiffuseIntensity", 0.35f);
    out.iblSpecularIntensity = s.value("iblSpecularIntensity", 1.0f);
    out.aoEnabled = s.value("aoEnabled", true);
    out.aoRadius = s.value("aoRadius", 0.75f);
    out.aoIntensity = s.value("aoIntensity", 1.0f);
    out.aoPower = s.value("aoPower", 1.35f);
    out.fogEnabled = s.value("fogEnabled", false);
    if (s.contains("fogColor"))
        out.fogColor = glm::vec4(jsonToVec3(s.value("fogColor", json())), 1.0f);
    out.fogStart = s.value("fogStart", 8.0f);
    out.fogDensity = s.value("fogDensity", 0.035f);
    out.bloomEnabled = s.value("bloomEnabled", true);
    out.bloomThreshold = s.value("bloomThreshold", 1.0f);
    out.bloomIntensity = s.value("bloomIntensity", 0.25f);
    out.bloomRadius = s.value("bloomRadius", 3.0f);
    out.changeRenderingAtLoad = s.value("changeRenderingAtLoad", true);
}

} // namespace

std::string serializeSceneSnapshot(Scene& scene, ResourceManager& resources) {
    json doc;
    format::writeSchema(doc, format::kSceneVersion);
    scene.serialize(doc["scene"], resources);
    return doc.dump(2);
}

std::string serializeSceneSnapshot(Scene& scene, ResourceManager* resources) {
    if (resources) return serializeSceneSnapshot(scene, *resources);

    json doc;
    format::writeSchema(doc, format::kSceneVersion);
    doc["scene"] = serializeSceneWithoutResources(scene);
    doc["snapshotMode"] = "authoring-headless";
    return doc.dump(2);
}

bool deserializeSceneSnapshot(const json& doc, Scene& out, std::string* error) {
    auto fail = [&](const std::string& m) {
        out.clearChildren();
        out.clearConnections();
        out.connections().clear();
        out.settings() = SceneSettings{};
        if (error) *error = m;
        return false;
    };

    if (error) error->clear();

    // Leave a clean, empty scene behind on any failure (never a half-built one).
    out.clearChildren();
    out.clearConnections();
    out.connections().clear();
    out.settings() = SceneSettings{};

    if (const std::string envelope =
            format::schemaEnvelopeError(doc, format::kSceneVersion, "snapshot");
        !envelope.empty())
        return fail(envelope);

    auto it = doc.find("scene");
    if (it == doc.end() || !it->is_object())
        return fail("snapshot document needs a 'scene' object");
    const json& root = *it;
    if (!root.contains("type") || !root["type"].is_string() || root["type"] != "Scene")
        return fail("snapshot root must have type 'Scene'");

    try {
        ensureHeadlessRegistries();
        std::string detail;
        if (!loadNodeFields(out, root, "scene", detail)) return fail(detail);
        if (!root.contains("id")) out.regenerateId();

        if (auto sit = root.find("settings"); sit != root.end()) {
            if (!sit->is_object()) return fail("scene.settings: expected an object");
            loadSceneSettings(out, *sit);
        }

        if (auto cit = root.find("children"); cit != root.end()) {
            if (!cit->is_array()) return fail("scene.children: expected an array");
            for (std::size_t i = 0; i < cit->size(); ++i) {
                const std::string path = "scene.children[" + std::to_string(i) + "]";
                std::unique_ptr<Node> child = deserializeNodeHeadless((*cit)[i], path, detail);
                if (!child) return fail(detail);
                out.addChild(std::move(child));
            }
        }

        if (auto connections = root.find("connections"); connections != root.end()) {
            if (!connections->is_array()) return fail("scene.connections: expected an array");
            for (std::size_t i = 0; i < connections->size(); ++i) {
                const json& connection = (*connections)[i];
                const std::string path =
                    "scene.connections[" + std::to_string(i) + "]";
                if (!connection.is_object()) return fail(path + ": expected an object");
                NodeId from = kNodeInvalid;
                NodeId to = kNodeInvalid;
                if (!connection.contains("from") ||
                    !snapshotNodeIdFromJson(connection["from"], from))
                    return fail(path + ".from: expected a non-zero decimal string");
                if (!connection.contains("to") ||
                    !snapshotNodeIdFromJson(connection["to"], to))
                    return fail(path + ".to: expected a non-zero decimal string");
                if (!connection.contains("signal") || !connection["signal"].is_string() ||
                    connection["signal"].get_ref<const std::string&>().empty())
                    return fail(path + ".signal: expected a non-empty string");
                if (!connection.contains("slot") || !connection["slot"].is_string() ||
                    connection["slot"].get_ref<const std::string&>().empty())
                    return fail(path + ".slot: expected a non-empty string");
                out.connections().push_back({from, connection["signal"].get<std::string>(),
                                             to, connection["slot"].get<std::string>()});
            }
        }
        Node::g_hierarchyVersion++;
        Node::g_transformVersion++;
        return true;
    } catch (const std::exception& e) {
        return fail(std::string("invalid snapshot data: ") + e.what());
    }
}

bool deserializeSceneSnapshot(const std::string& text, Scene& out, std::string* error) {
    json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) {
        if (error) *error = "invalid JSON";
        out.clearChildren();
        return false;
    }
    return deserializeSceneSnapshot(doc, out, error);
}

} // namespace saida::authoring
