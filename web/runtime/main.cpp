// Web runtime — Step 16: the real engine Renderer running on the rhi::webgpu
// backend, loading the actual BeachDemo scene (scenes/beach.scene, preloaded
// into MEMFS at /project). This is the desktop-vs-web parity check: same scene
// data, same drawFrame — shadows + DDGI + water + skybox + AO + bloom + tonemap.
//
// A focused JSON loader (nlohmann) builds the node types the scene uses —
// MeshNode / LightNode / WaterNode / Camera — so the web build stays free of the
// editor's full SceneSerializer/reflection/GLTF stack.

#include "rhi/webgpu/RhiWeb.hpp"
#include "authoring/EngineManifest.hpp"
#include "authoring/SceneSnapshot.hpp"
#include "authoring/SaidaOpApplier.hpp"
#include "graphics/Material.hpp"
#include "graphics/Mesh.hpp"
#include "graphics/ResourceManager.hpp"
#include "project/AssetRegistry.hpp"
#include "render/Renderer.hpp"
#include "runtime/RuntimeRoundTripContract.hpp"
#include "render/RenderFeatureRegistry.hpp"
#include "core/Camera.hpp"
#include "core/FormatVersions.hpp"
#include "core/Reflection.hpp"
#include "core/Time.hpp"
#include "nodes/CameraNode.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/MeshNode.hpp"
#include "nodes/ParticleSystemNode.hpp"
#include "nodes/WaterNode.hpp"
#include "scene/Scene.hpp"

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <GLFW/glfw3.h>
#include <emscripten.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace saida;
using json = nlohmann::json;

namespace {

constexpr uint32_t kWidth = 1280, kHeight = 720;

struct App {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<rhi::Surface> surface;
    // Maps AssetID <-> project-relative path (asset_registry.json when the
    // platform preloads one under /project); path refs register on the fly.
    std::unique_ptr<AssetRegistry> registry;
    std::unique_ptr<ResourceManager> resources;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<Scene> scene;
    Camera camera;
    Mesh* cubeMesh = nullptr;
    AssetID whiteTex = kAssetInvalid;
    AssetID durableSkyboxTexture = kAssetInvalid;
    // Track 1-F: refs the last scene load could not resolve (missing files in
    // MEMFS, ids unknown to the registry). Reported by saida_load_snapshot so
    // the editor can surface them instead of silently rendering placeholders.
    std::vector<std::string> missingAssets;
    bool ready = false;
    std::string startupError;
    double time = 0.0;
    // Editor graphics settings (saida_set_render_settings): a frame-skip cap on
    // top of the rAF loop. 0 = uncapped (every rAF tick renders).
    double minFrameIntervalMs = 0.0;
    double lastFrameMs = 0.0;
};

App gApp;

void loadNode(const json& node, Node& parent, const std::string& path);
bool loadSceneDoc(const json& doc, std::string& error);

// ---- JSON readers (arrays → glm) ----

glm::vec3 readVec3(const json& a, glm::vec3 fallback = glm::vec3(0.0f)) {
    if (!a.is_array() || a.size() < 3) return fallback;
    return {a[0].get<float>(), a[1].get<float>(), a[2].get<float>()};
}
glm::vec4 readVec4(const json& a, glm::vec4 fallback = glm::vec4(0.0f)) {
    if (!a.is_array() || a.size() < 4) return fallback;
    return {a[0].get<float>(), a[1].get<float>(), a[2].get<float>(), a[3].get<float>()};
}
// Scene stores quaternions as [x, y, z, w]; glm::quat is (w, x, y, z).
glm::quat readQuat(const json& a, glm::quat fallback = glm::quat(1, 0, 0, 0)) {
    if (!a.is_array() || a.size() < 4) return fallback;
    return {a[3].get<float>(), a[0].get<float>(), a[1].get<float>(), a[2].get<float>()};
}
float readF(const json& o, const char* key, float d) {
    return o.contains(key) ? o[key].get<float>() : d;
}

void applyTransform(Node& n, const json& node) {
    if (!node.contains("transform")) return;
    const json& t = node["transform"];
    n.transform().position = readVec3(t.value("position", json::array()), glm::vec3(0.0f));
    n.transform().rotation = readQuat(t.value("rotation", json::array()));
    n.transform().scale = readVec3(t.value("scale", json::array()), glm::vec3(1.0f));
}

void applyCommonNodeFields(Node& n, const json& node) {
    if (node.contains("id") && node["id"].is_number_unsigned())
        n.assignSerializedId(node["id"].get<NodeId>());
    if (node.contains("name") && node["name"].is_string())
        n.setName(node["name"].get<std::string>());
    if (node.contains("enabled") && node["enabled"].is_boolean())
        n.setEnabled(node["enabled"].get<bool>());
    if (node.contains("importedFrom") && node["importedFrom"].is_string())
        n.setImportedFromPath(node["importedFrom"].get<std::string>());
    if (node.contains("groups") && node["groups"].is_array()) {
        for (const json& group : node["groups"])
            if (group.is_string()) n.addToGroup(group.get<std::string>());
    }
    applyTransform(n, node);
}

// Track 1-F: resolve an asset ref as serialized by the engine — an AssetID
// number, or a project-relative path string (registered on the fly). Returns
// kAssetInvalid when there is no usable ref.
AssetID resolveAssetRef(const json& ref, AssetType type) {
    if (ref.is_number_unsigned() || ref.is_number_integer())
        return ref.get<AssetID>();
    if (ref.is_string()) {
        const std::string s = ref.get<std::string>();
        if (s.empty()) return kAssetInvalid;
        if (s == "cube") return kAssetBuiltinCube;
        return gApp.resources->getOrRegister(s, type);
    }
    return kAssetInvalid;
}

std::string describeAssetRef(const json& ref) {
    return ref.is_string() ? ref.get<std::string>() : ref.dump();
}

bool fileExists(const std::string& path) {
    struct stat st {};
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

// Resolve a MeshNode's "mesh" ref to a live Mesh. Missing/unresolvable refs are
// recorded in gApp.missingAssets (the caller falls back to the placeholder cube
// so the scene stays visible, but the miss is explicit — never silent).
Mesh* resolveMeshRef(const json& node) {
    if (!node.contains("mesh")) return nullptr;  // authored without a mesh ref
    const AssetID id = resolveAssetRef(node["mesh"], AssetType::Mesh);
    if (id != kAssetInvalid) {
        if (id == kAssetBuiltinCube) return gApp.resources->getMesh(id);
        // Path-backed mesh: only ask the loader when the file is actually in
        // MEMFS — Mesh::fromObjFile throws on a missing file, and a C++ throw
        // aborts the wasm runtime (exceptions are disabled on web).
        const std::string path = gApp.registry ? gApp.registry->getPath(id) : std::string();
        if (fileExists(path)) {
            if (Mesh* mesh = gApp.resources->getMesh(id)) return mesh;
        }
    }
    const std::string label = "mesh:" + describeAssetRef(node["mesh"]);
    gApp.missingAssets.push_back(label);
    std::printf("saida-web: unresolved %s (node '%s') — placeholder cube\n",
                label.c_str(), node.value("name", std::string("?")).c_str());
    return nullptr;
}

// Build a Lit material from a MeshNode's inline PBR fields.
Material* materialFromNode(const json& node) {
    MaterialDesc d;
    // texture:0 = durable "none", resolved to default white.
    if (node.contains("texture")) {
        d.albedoId = resolveAssetRef(node["texture"], AssetType::Texture);
        // Keep the durable id in the desc even when unresolved (the Material
        // falls back to white and the id re-serializes unchanged); just report.
        if (d.albedoId != kAssetInvalid && !gApp.resources->getTexture(d.albedoId))
            gApp.missingAssets.push_back("texture:" + describeAssetRef(node["texture"]));
    }
    d.baseColor = readVec4(node.value("baseColor", json::array()), glm::vec4(1.0f));
    d.emissiveColor = readVec4(node.value("emissive", json::array()), glm::vec4(0.0f));
    d.metallic = readF(node, "metallic", 0.0f);
    d.roughness = readF(node, "roughness", 0.5f);
    d.ao = readF(node, "ao", 1.0f);
    if (node.value("shader", std::string("lit")) == "unlit") d.type = MaterialType::Unlit;
    return gApp.resources->getMaterial(d);
}

void loadWater(const json& node, Node& parent, const std::string& path) {
    auto w = std::make_unique<WaterNode>();
    applyCommonNodeFields(*w, node);
    w->size = readF(node, "size", w->size);
    w->deepColor = readVec3(node.value("deepColor", json::array()), w->deepColor);
    w->foamColor = readVec3(node.value("foamColor", json::array()), w->foamColor);
    w->shallowColor = readVec3(node.value("shallowColor", json::array()), w->shallowColor);
    w->roughness = readF(node, "roughness", w->roughness);
    w->reflectivity = readF(node, "reflectivity", w->reflectivity);
    w->amplitude = readF(node, "amplitude", w->amplitude);
    w->wavelength = readF(node, "wavelength", w->wavelength);
    w->waveSpeed = readF(node, "waveSpeed", w->waveSpeed);
    w->choppiness = readF(node, "choppiness", w->choppiness);
    w->detailScale = readF(node, "detailScale", w->detailScale);
    w->detailSpeed = readF(node, "detailSpeed", w->detailSpeed);
    w->detailStrength = readF(node, "detailStrength", w->detailStrength);
    w->detailAngle = readF(node, "detailAngle", w->detailAngle);
    w->detail2Scale = readF(node, "detail2Scale", w->detail2Scale);
    w->detail2Speed = readF(node, "detail2Speed", w->detail2Speed);
    w->detail2Strength = readF(node, "detail2Strength", w->detail2Strength);
    w->detail2Angle = readF(node, "detail2Angle", w->detail2Angle);
    w->fresnelPower = readF(node, "fresnelPower", w->fresnelPower);
    w->specularPower = readF(node, "specularPower", w->specularPower);
    w->specularIntensity = readF(node, "specularIntensity", w->specularIntensity);
    w->foamThreshold = readF(node, "foamThreshold", w->foamThreshold);
    w->foamIntensity = readF(node, "foamIntensity", w->foamIntensity);
    w->warpAmount = readF(node, "warpAmount", w->warpAmount);
    w->detailFadeDistance = readF(node, "detailFadeDistance", w->detailFadeDistance);
    w->depthColorFalloff = readF(node, "depthColorFalloff", w->depthColorFalloff);
    w->edgeFade = readF(node, "edgeFade", w->edgeFade);
    w->shoreSlope = readF(node, "shoreSlope", w->shoreSlope);
    w->shoreMode = static_cast<WaterNode::ShoreMode>(
        static_cast<int>(readF(node, "shoreMode", 0.0f)));
    w->shoreAngle = readF(node, "shoreAngle", w->shoreAngle);
    w->shoreWaterline = readF(node, "shoreWaterline", w->shoreWaterline);
    w->lakeRadius = readF(node, "lakeRadius", w->lakeRadius);
    w->shoreFoam = readF(node, "shoreFoam", w->shoreFoam);
    w->foamWidth = readF(node, "foamWidth", w->foamWidth);
    w->swashSpeed = readF(node, "swashSpeed", w->swashSpeed);
    w->swashAmount = readF(node, "swashAmount", w->swashAmount);
    w->waveFlatten = readF(node, "waveFlatten", w->waveFlatten);
    Node& ref = *w;
    parent.addChild(std::move(w));
    const json& children = node.value("children", json::array());
    for (std::size_t i = 0; i < children.size(); ++i)
        loadNode(children[i], ref, path + ".children[" + std::to_string(i) + "]");
}

void loadLight(const json& node, Node& parent, const std::string& path) {
    const auto type = static_cast<LightType>(node.value("lightType", 0));
    auto l = std::make_unique<LightNode>(node.value("name", std::string("Light")), type);
    applyCommonNodeFields(*l, node);
    l->color = readVec3(node.value("color", json::array()), glm::vec3(1.0f));
    l->intensity = readF(node, "intensity", 1.0f);
    l->range = readF(node, "range", l->range);
    l->direction = readVec3(node.value("direction", json::array()), glm::vec3(0, -1, 0));
    l->spotInnerAngle = readF(node, "spotInnerAngle", l->spotInnerAngle);
    l->spotOuterAngle = readF(node, "spotOuterAngle", l->spotOuterAngle);
    l->castShadows = node.value("castShadows", false);
    l->bakeMode = static_cast<LightBakeMode>(node.value("bakeMode", 0));
    Node& ref = *l;
    parent.addChild(std::move(l));
    const json& children = node.value("children", json::array());
    for (std::size_t i = 0; i < children.size(); ++i)
        loadNode(children[i], ref, path + ".children[" + std::to_string(i) + "]");
}

void applyCamera(const json& node) {
    glm::vec3 pos(0.0f);
    glm::quat rot(1, 0, 0, 0);
    if (node.contains("transform")) {
        pos = readVec3(node["transform"].value("position", json::array()), pos);
        rot = readQuat(node["transform"].value("rotation", json::array()));
    }
    gApp.camera.position = pos;
    // Camera looks down -Z in its local frame; rotate that into world space.
    const glm::vec3 forward = glm::normalize(rot * glm::vec3(0.0f, 0.0f, -1.0f));
    gApp.camera.lookAt(pos + forward);
    gApp.camera.fovDegrees = readF(node, "fovDegrees", 60.0f);
    gApp.camera.nearZ = readF(node, "nearZ", 0.1f);
    gApp.camera.farZ = readF(node, "farZ", 600.0f);
}

void loadCamera(const json& node, Node& parent, const std::string& path) {
    auto c = std::make_unique<CameraNode>(node.value("name", std::string("Camera")));
    applyCommonNodeFields(*c, node);
    c->fovDegrees = readF(node, "fovDegrees", c->fovDegrees);
    c->nearZ = readF(node, "nearZ", c->nearZ);
    c->farZ = readF(node, "farZ", c->farZ);
    c->priority = node.value("priority", c->priority);
    c->active = node.value("active", c->active);
    Node& ref = *c;
    parent.addChild(std::move(c));
    applyCamera(node);
    const json& children = node.value("children", json::array());
    for (std::size_t i = 0; i < children.size(); ++i)
        loadNode(children[i], ref, path + ".children[" + std::to_string(i) + "]");
}

void loadParticle(const json& node, Node& parent, const std::string& path) {
    auto particle = std::make_unique<ParticleSystemNode>();
    applyCommonNodeFields(*particle, node);
    reflect::localDesc<ParticleSystemNode>().loadFrom(particle.get(), node);
    Node& ref = *particle;
    parent.addChild(std::move(particle));
    const json& children = node.value("children", json::array());
    for (std::size_t i = 0; i < children.size(); ++i)
        loadNode(children[i], ref, path + ".children[" + std::to_string(i) + "]");
}

void loadNode(const json& node, Node& parent, const std::string& path) {
    const std::string type = node.value("type", "");

    if (type == "MeshNode") {
        // Track 1-F: resolve the durable mesh ref (AssetID or path) to a real
        // mesh; unresolved refs fall back to the placeholder cube and are
        // reported in missingAssets — visible, never silent.
        Mesh* mesh = resolveMeshRef(node);
        auto m = std::make_unique<MeshNode>(node.value("name", std::string("Mesh")),
                                            mesh ? mesh : gApp.cubeMesh,
                                            materialFromNode(node));
        // Keep the serialized identity on the node so authoring paths that
        // re-serialize headless never lose the refs.
        m->captureDurableResourceRefs(node);
        applyCommonNodeFields(*m, node);
        m->castShadows() = node.value("castShadows", true);
        if (node.contains("meshEnabled")) m->setMeshEnabled(node["meshEnabled"].get<bool>());
        if (node.contains("outlineEnabled")) m->setOutlineEnabled(node["outlineEnabled"].get<bool>());
        if (node.contains("outlineColor")) m->outlineColor() = readVec4(node["outlineColor"], m->outlineColor());
        if (node.contains("outlineWidth")) m->outlineWidth() = node["outlineWidth"].get<float>();
        Node& ref = *m;
        parent.addChild(std::move(m));
        const json& children = node.value("children", json::array());
        for (std::size_t i = 0; i < children.size(); ++i)
            loadNode(children[i], ref, path + ".children[" + std::to_string(i) + "]");
        return;
    }
    if (type == "Water")  { loadWater(node, parent, path); return; }
    if (type == "LightNode") { loadLight(node, parent, path); return; }
    if (type == "Camera")  { loadCamera(node, parent, path); return; }
    if (type == "ParticleSystem") { loadParticle(node, parent, path); return; }

    if (type != "Node")
        throw std::runtime_error(path + ": unsupported web node type '" + type + "'");

    auto generic = std::make_unique<Node>(node.value("name", std::string("Node")));
    applyCommonNodeFields(*generic, node);
    Node& ref = *generic;
    parent.addChild(std::move(generic));
    const json& children = node.value("children", json::array());
    for (std::size_t i = 0; i < children.size(); ++i)
        loadNode(children[i], ref, path + ".children[" + std::to_string(i) + "]");
}

void applySettings(const json& s) {
    SceneSettings& out = gApp.scene->settings();
    out.ambientLight = glm::vec4(readVec3(s.value("ambient", json::array()), glm::vec3(0.1f)), 1.0f);
    out.clearColor = glm::vec4(readVec3(s.value("clearColor", json::array()), glm::vec3(0.0f)), 1.0f);
    out.enablePostProcessing = s.value("postProcessing", true);
    out.aoEnabled = s.value("aoEnabled", true);
    out.aoRadius = readF(s, "aoRadius", out.aoRadius);
    out.aoIntensity = readF(s, "aoIntensity", out.aoIntensity);
    out.aoPower = readF(s, "aoPower", out.aoPower);
    out.bloomEnabled = s.value("bloomEnabled", true);
    out.bloomThreshold = readF(s, "bloomThreshold", out.bloomThreshold);
    out.bloomIntensity = readF(s, "bloomIntensity", out.bloomIntensity);
    out.bloomRadius = readF(s, "bloomRadius", out.bloomRadius);
    out.changeRenderingAtLoad = s.value("changeRenderingAtLoad", out.changeRenderingAtLoad);
    out.fogEnabled = s.value("fogEnabled", false);
    out.fogColor = glm::vec4(readVec3(s.value("fogColor", json::array()), glm::vec3(0.5f)), 1.0f);
    out.fogStart = readF(s, "fogStart", out.fogStart);
    out.fogDensity = readF(s, "fogDensity", out.fogDensity);
    out.lightingMode = static_cast<LightingMode>(s.value("lightingMode", 0));
    out.giMode = static_cast<GIMode>(s.value("giMode", 0));
    out.giEnabled = s.value("giEnabled", true);
    out.giIntensity = readF(s, "giIntensity", out.giIntensity);
    out.iblEnabled = s.value("iblEnabled", true);
    out.iblDiffuseIntensity = readF(s, "iblDiffuseIntensity", out.iblDiffuseIntensity);
    out.iblSpecularIntensity = readF(s, "iblSpecularIntensity", out.iblSpecularIntensity);
    out.skyboxExposure = readF(s, "skyboxExposure", 1.0f);
    out.skyboxRotation = readF(s, "skyboxRotation", 0.0f);
}

// Loads /project/sunset.png (preloaded into MEMFS) as the skybox + IBL source.
AssetID loadSkyTexture() {
    int w = 0, h = 0, ch = 0;
    stbi_uc* px = stbi_load("/project/sunset.png", &w, &h, &ch, 4);
    if (!px) {
        std::printf("saida-web: sunset.png not found — sky stays black\n");
        return kAssetInvalid;
    }
    const AssetID id = gApp.resources->registerGeneratedTexture(
        px, uint32_t(w), uint32_t(h), rhi::Format::RGBA8Srgb, true);
    stbi_image_free(px);
    std::printf("saida-web: loaded sunset.png %dx%d\n", w, h);
    return id;
}

std::string durableSceneSnapshotJson() {
    json doc = json::parse(saida::authoring::serializeSceneSnapshot(*gApp.scene, *gApp.resources));
    if (gApp.durableSkyboxTexture != kAssetInvalid &&
        doc.contains("scene") && doc["scene"].contains("settings")) {
        doc["scene"]["settings"]["skyboxTexture"] = gApp.durableSkyboxTexture;
    }
    return doc.dump(2);
}

void canonicalizeReferenceNode(json& node) {
    const std::string type = node.value("type", "");
    node.erase("includeInLightBaking");
    if (type == "MeshNode") {
        if (!node.contains("meshEnabled")) node["meshEnabled"] = true;
        if (!node.contains("castShadows")) node["castShadows"] = true;
        if (!node.contains("outlineEnabled")) node["outlineEnabled"] = false;
        if (!node.contains("outlineColor")) node["outlineColor"] = json::array({0.02f, 0.02f, 0.02f, 1.0f});
        if (!node.contains("outlineWidth")) node["outlineWidth"] = 3.0f;
    } else if (type == "LightNode") {
        if (!node.contains("range")) node["range"] = 10.0f;
        if (!node.contains("spotInnerAngle")) node["spotInnerAngle"] = 25.0f;
        if (!node.contains("spotOuterAngle")) node["spotOuterAngle"] = 35.0f;
        if (!node.contains("castShadows")) node["castShadows"] = true;
        if (!node.contains("bakeMode")) node["bakeMode"] = 0;
    } else if (type == "Camera") {
        if (!node.contains("groups")) node["groups"] = json::array({"camera"});
        if (!node.contains("priority")) node["priority"] = 0;
        if (!node.contains("active")) node["active"] = true;
    }

    if (!node.contains("behaviours")) node["behaviours"] = json::array();
    if (!node.contains("children")) node["children"] = json::array();
    for (json& child : node["children"]) canonicalizeReferenceNode(child);
}

void canonicalizeReferenceDoc(json& doc) {
    if (doc.contains("scene")) canonicalizeReferenceNode(doc["scene"]);
}

bool firstJsonDiff(const json& actual, const json& expected,
                   const std::string& path, std::string& out) {
    if (actual.type() != expected.type()) {
        out = path + " type differs";
        return true;
    }
    if (actual.is_object()) {
        for (auto it = actual.begin(); it != actual.end(); ++it) {
            if (!expected.contains(it.key())) {
                out = path + "." + it.key() + " missing from expected";
                return true;
            }
            if (firstJsonDiff(it.value(), expected.at(it.key()), path + "." + it.key(), out))
                return true;
        }
        for (auto it = expected.begin(); it != expected.end(); ++it) {
            if (!actual.contains(it.key())) {
                out = path + "." + it.key() + " missing from actual";
                return true;
            }
        }
        return false;
    }
    if (actual.is_array()) {
        if (actual.size() != expected.size()) {
            out = path + " size differs: actual " + std::to_string(actual.size()) +
                  ", expected " + std::to_string(expected.size());
            return true;
        }
        for (size_t i = 0; i < actual.size(); ++i) {
            if (firstJsonDiff(actual[i], expected[i], path + "[" + std::to_string(i) + "]", out))
                return true;
        }
        return false;
    }
    if (actual.is_number() && expected.is_number()) {
        if (actual.is_number_float() || expected.is_number_float()) {
            const double av = actual.get<double>();
            const double ev = expected.get<double>();
            if (std::fabs(av - ev) <= 0.00001) return false;
        }
    }
    if (actual != expected) {
        out = path + " differs: actual=" + actual.dump() + ", expected=" + expected.dump();
        return true;
    }
    return false;
}

std::string compareSnapshotWithPreloadedScene() {
    json result;
    if (!gApp.ready || !gApp.scene || !gApp.resources) {
        result["ok"] = false;
        result["error"] = "runtime not ready";
        return result.dump();
    }

    std::ifstream in("/project/beach.scene");
    if (!in) {
        result["ok"] = false;
        result["error"] = "reference scene missing";
        return result.dump();
    }

    try {
        json expected;
        in >> expected;
        canonicalizeReferenceDoc(expected);
        json actual = json::parse(durableSceneSnapshotJson());
        std::string diff;
        const bool differs = firstJsonDiff(actual, expected, "$", diff);
        result["ok"] = !differs;
        result["reference"] = "/project/beach.scene";
        result["actualChildren"] = actual["scene"].value("children", json::array()).size();
        result["expectedChildren"] = expected["scene"].value("children", json::array()).size();
        result["runtimeSkyboxTexture"] = gApp.scene->settings().skyboxTexture;
        result["durableSkyboxTexture"] = gApp.durableSkyboxTexture;
        if (differs) result["firstDiff"] = diff;
    } catch (const std::exception& e) {
        result["ok"] = false;
        result["error"] = e.what();
    }
    return result.dump(2);
}

void initRuntime() {
    rhi::Device& device = *gApp.device;
    registerBuiltinRenderFeatures();  // water + skybox + particles on web

    // Track 1-F: asset identity for the project mounted at /project. The
    // registry maps AssetID <-> relative path (asset_registry.json when the
    // platform ships one); chdir makes registry-relative paths (meshes,
    // textures) resolve under /project. Engine-internal paths stay absolute
    // (/shaders, /assets), so the cwd change is safe.
    gApp.registry = std::make_unique<AssetRegistry>();
    gApp.registry->load("/project");
    chdir("/project");
    std::printf("saida-web: asset registry ready (root /project)\n");

    gApp.resources = std::make_unique<ResourceManager>(device, gApp.registry.get());
    std::printf("saida-web: resources ready\n");
    gApp.cubeMesh = gApp.resources->getMesh(kAssetBuiltinCube);

    const uint8_t white[4] = {255, 255, 255, 255};
    gApp.whiteTex = gApp.resources->registerGeneratedTexture(white, 1, 1, rhi::Format::RGBA8Srgb, false);

    std::ifstream in("/project/beach.scene");
    if (!in) { std::printf("saida-web: beach.scene missing\n"); return; }
    json doc;
    in >> doc;
    std::string loadError;
    if (!loadSceneDoc(doc, loadError)) {
        std::printf("saida-web: beach.scene rejected: %s\n", loadError.c_str());
        return;
    }
    const AssetID skyId = loadSkyTexture();
    if (skyId != kAssetInvalid) gApp.scene->settings().skyboxTexture = skyId;
    gApp.renderer = std::make_unique<Renderer>(device, *gApp.surface, *gApp.resources);
    std::printf("saida-web: BeachDemo loaded — %zu meshes, %zu waters, %zu lights\n",
                gApp.scene->meshes().size(), gApp.scene->waterNodes().size(),
                gApp.scene->lights().size());
}

void frame() {
    float dt = 1.0f / 60.0f;
    if (gApp.minFrameIntervalMs > 0.0) {
        // FPS cap = frame skipping over rAF (smoother than setTimeout timing).
        // Rendered frames advance time by the real elapsed interval so capped
        // animation keeps wall-clock speed (clamped against tab-suspend jumps).
        const double now = emscripten_get_now();
        if (now - gApp.lastFrameMs < gApp.minFrameIntervalMs) return;
        if (gApp.lastFrameMs > 0.0)
            dt = float(std::min(now - gApp.lastFrameMs, 250.0) / 1000.0);
        gApp.lastFrameMs = now;
    }
    gApp.time += dt;
    Time::advance(dt);  // the water/skybox read Time::elapsed() for animation
    if (gApp.scene && gApp.renderer) {
        gApp.scene->update(dt);
        gApp.renderer->drawFrame(*gApp.scene, gApp.camera, nullptr);
    }
}

// Rebuild the live authoring scene from a
// scene-document JSON (the `doc` returned by the platform's GET /scene). Reuses
// the same focused loaders as boot (loadNode/applySettings) so a snapshot round-
// trips into the exact node types the web runtime can render. Replacing the Scene
// unique_ptr is safe: the Renderer is scene-agnostic (scene passed per drawFrame),
// and this runs synchronously from JS between frames (single-threaded main loop).
//
// Track 1-F: headless snapshots now carry mesh/material refs (SceneSnapshot.cpp)
// and this loader resolves them — .obj meshes + textures load from the project
// files mounted at /project; unresolved refs render as placeholder cubes and are
// reported in the saida_load_snapshot result (missingAssets).
bool isSupportedWebNodeType(const std::string& type) {
    return type == "Node" || type == "MeshNode" || type == "LightNode" ||
           type == "Water" || type == "Camera" || type == "ParticleSystem";
}

bool validateWebNode(const json& node, const std::string& path, std::string& error) {
    if (!node.is_object() || !node.contains("type") || !node["type"].is_string()) {
        error = path + ": node needs a string 'type'";
        return false;
    }
    const std::string type = node["type"].get<std::string>();
    if (!isSupportedWebNodeType(type)) {
        error = path + ": unsupported web node type '" + type + "'";
        return false;
    }
    if (auto behaviours = node.find("behaviours"); behaviours != node.end()) {
        if (!behaviours->is_array()) {
            error = path + ".behaviours: expected an array";
            return false;
        }
        if (!behaviours->empty()) {
            const json& first = (*behaviours)[0];
            const std::string typeName = first.is_object() && first.contains("type") &&
                                                 first["type"].is_string()
                                             ? first["type"].get<std::string>()
                                             : "<invalid>";
            error = path + ".behaviours[0]: unsupported web behaviour type '" +
                    typeName + "'";
            return false;
        }
    }
    if (auto children = node.find("children"); children != node.end()) {
        if (!children->is_array()) {
            error = path + ".children: expected an array";
            return false;
        }
        for (std::size_t i = 0; i < children->size(); ++i) {
            if (!validateWebNode((*children)[i],
                                 path + ".children[" + std::to_string(i) + "]", error))
                return false;
        }
    }
    return true;
}

bool validateWebSceneDoc(const json& doc, std::string& error) {
    if (!doc.is_object() || !doc.contains("scene") || !doc["scene"].is_object()) {
        error = "invalid scene document: missing scene root";
        return false;
    }
    if (const std::string envelope =
            format::schemaEnvelopeError(doc, format::kSceneVersion, "snapshot");
        !envelope.empty()) {
        error = envelope;
        return false;
    }

    const json& root = doc["scene"];
    if (!root.contains("type") || !root["type"].is_string() ||
        root["type"].get<std::string>() != "Scene") {
        error = "scene: root node type must be 'Scene'";
        return false;
    }
    if (root.contains("settings") && !root["settings"].is_object()) {
        error = "scene.settings: expected an object";
        return false;
    }
    if (root.contains("connections") && !root["connections"].is_array()) {
        error = "scene.connections: expected an array";
        return false;
    }
    if (auto behaviours = root.find("behaviours"); behaviours != root.end() &&
        (!behaviours->is_array() || !behaviours->empty())) {
        error = "scene.behaviours: web runtime does not support behaviours";
        return false;
    }
    if (auto children = root.find("children"); children != root.end()) {
        if (!children->is_array()) {
            error = "scene.children: expected an array";
            return false;
        }
        for (std::size_t i = 0; i < children->size(); ++i) {
            if (!validateWebNode((*children)[i],
                                 "scene.children[" + std::to_string(i) + "]", error))
                return false;
        }
    }
    return true;
}

bool loadSceneDoc(const json& doc, std::string& error) {
    if (!validateWebSceneDoc(doc, error)) return false;

    std::unique_ptr<Scene> previousScene = std::move(gApp.scene);
    const Camera previousCamera = gApp.camera;
    const AssetID previousSkyboxTexture = gApp.durableSkyboxTexture;
    std::vector<std::string> previousMissingAssets = std::move(gApp.missingAssets);

    try {
        gApp.scene = std::make_unique<Scene>();
        gApp.camera = Camera{};
        gApp.durableSkyboxTexture = kAssetInvalid;
        gApp.missingAssets.clear();

        const json& root = doc["scene"];
        applyCommonNodeFields(*gApp.scene, root);
        if (root.contains("settings")) {
            applySettings(root["settings"]);
            gApp.durableSkyboxTexture =
                root["settings"].value("skyboxTexture", kAssetInvalid);
            if (gApp.durableSkyboxTexture != kAssetInvalid && gApp.whiteTex != kAssetInvalid)
                gApp.scene->settings().skyboxTexture = gApp.durableSkyboxTexture;
        }

        gApp.scene->readConnections(root);
        const json& children = root.value("children", json::array());
        for (std::size_t i = 0; i < children.size(); ++i)
            loadNode(children[i], *gApp.scene,
                     "scene.children[" + std::to_string(i) + "]");
        gApp.scene->update(0.0f);
        return true;
    } catch (const std::exception& e) {
        gApp.scene = std::move(previousScene);
        gApp.camera = previousCamera;
        gApp.durableSkyboxTexture = previousSkyboxTexture;
        gApp.missingAssets = std::move(previousMissingAssets);
        error = e.what();
        return false;
    }
}

// D9 (Track 1-C): viewport picking. No physics/Jolt in the web build, so we ray-
// cast against each MeshNode's local AABB (Mesh::bounds()) transformed by its
// world matrix, and keep the nearest hit. Enough for click-to-select; precise
// per-triangle picking can come later.
bool rayAabbLocal(const glm::vec3& o, const glm::vec3& d, const Aabb& b, float& tNear) {
    float t0 = 0.0f;
    float t1 = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-8f) {
            if (o[i] < b.min[i] || o[i] > b.max[i]) return false;  // parallel, outside
        } else {
            const float inv = 1.0f / d[i];
            float ta = (b.min[i] - o[i]) * inv;
            float tb = (b.max[i] - o[i]) * inv;
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            if (t0 > t1) return false;
        }
    }
    tNear = t0;
    return true;
}

// ndcX/ndcY in [-1, 1] with Y up (the JS caller converts from canvas pixels).
std::string pickNode(float ndcX, float ndcY) {
    json r;
    if (!gApp.scene) {
        r["ok"] = false;
        r["error"] = "runtime not ready";
        return r.dump();
    }

    const float aspect = float(kWidth) / float(kHeight);
    const float tanHalf = std::tan(glm::radians(gApp.camera.fovDegrees) * 0.5f);
    const glm::vec3 camPos = gApp.camera.position;
    // Build the world-space ray from the camera basis (avoids projection Y-flip
    // pitfalls of unprojecting through the Vulkan-convention matrices).
    const glm::vec3 dirWorld = glm::normalize(
        gApp.camera.front() + gApp.camera.right() * (ndcX * tanHalf * aspect) +
        gApp.camera.up() * (ndcY * tanHalf));

    float bestDist = 1e30f;
    Node* best = nullptr;
    for (MeshNode* mn : gApp.scene->meshes()) {
        const Mesh* mesh = mn->mesh();
        if (!mesh) continue;
        const glm::mat4 world = mn->worldTransform();
        const glm::mat4 inv = glm::inverse(world);
        const glm::vec3 lo = glm::vec3(inv * glm::vec4(camPos, 1.0f));
        const glm::vec3 ld = glm::normalize(glm::vec3(inv * glm::vec4(dirWorld, 0.0f)));
        float tN = 0.0f;
        if (!rayAabbLocal(lo, ld, mesh->bounds(), tN)) continue;
        const glm::vec3 worldHit = glm::vec3(world * glm::vec4(lo + ld * tN, 1.0f));
        const float dist = glm::length(worldHit - camPos);
        if (dist < bestDist) {
            bestDist = dist;
            best = mn;
        }
    }

    r["ok"] = true;
    if (!best) {
        r["hit"] = false;
        return r.dump();
    }
    r["hit"] = true;
    r["nodeId"] = std::to_string(best->id());
    r["id"] = std::to_string(best->id());
    r["distance"] = bestDist;
    return r.dump();
}

} // namespace

// JSON string API exposed to JavaScript through Module.ccall.
extern "C" {

EMSCRIPTEN_KEEPALIVE
const char* saida_runtime_status() {
    static std::string out;
    json status;
    status["ready"] = gApp.ready;
    status["state"] = gApp.ready ? "ready" :
        (gApp.startupError.empty() ? "initializing" : "error");
    if (!gApp.startupError.empty()) status["error"] = gApp.startupError;
    out = status.dump();
    return out.c_str();
}

EMSCRIPTEN_KEEPALIVE
const char* saida_apply_op(const char* opJson) {
    static std::string out;
    if (!gApp.ready || !gApp.scene || !gApp.resources) {
        out = R"({"ok":false,"error":"runtime not ready"})";
        return out.c_str();
    }
    out = saida::authoring::applyOpJson(*gApp.scene, *gApp.resources,
                                        opJson ? opJson : "");
    return out.c_str();
}

EMSCRIPTEN_KEEPALIVE
const char* saida_engine_manifest() {
    static std::string m = saida::authoring::buildEngineManifest().dump();
    return m.c_str();
}

EMSCRIPTEN_KEEPALIVE
const char* saida_scene_snapshot() {
    static std::string out;
    if (!gApp.scene || !gApp.resources) {
        out = R"({"ok":false,"error":"runtime not ready"})";
        return out.c_str();
    }
    try {
        out = durableSceneSnapshotJson();
    } catch (const std::exception& e) {
        out = json{{"ok", false}, {"error", e.what()}}.dump();
    }
    return out.c_str();
}

EMSCRIPTEN_KEEPALIVE
const char* saida_scene_snapshot_compare() {
    static std::string out;
    out = compareSnapshotWithPreloadedScene();
    return out.c_str();
}

// D2 (Track 1-B): load a scene document into the live runtime, replacing the
// current scene. Input is the `doc` field of GET /v1/projects/:id/scene.
EMSCRIPTEN_KEEPALIVE
const char* saida_load_snapshot(const char* docJson) {
    static std::string out;
    if (!gApp.ready || !gApp.device || !gApp.resources) {
        out = R"({"ok":false,"error":"runtime not ready"})";
        return out.c_str();
    }
    try {
        json doc = json::parse(docJson ? docJson : "");
        std::string error;
        if (!loadSceneDoc(doc, error)) {
            out = json{{"ok", false}, {"error", error}}.dump();
            return out.c_str();
        }
        json r;
        r["ok"] = true;
        r["meshes"] = gApp.scene->meshes().size();
        r["lights"] = gApp.scene->lights().size();
        r["waters"] = gApp.scene->waterNodes().size();
        // Track 1-F: refs the loader could not resolve (placeholder rendered).
        r["missingAssets"] = gApp.missingAssets;
        out = r.dump();
    } catch (const std::exception& e) {
        json r;
        r["ok"] = false;
        r["error"] = e.what();
        out = r.dump();
    }
    return out.c_str();
}

// D9 (Track 1-C): pick the nearest MeshNode under a viewport point. ndcX/ndcY are
// normalized device coords in [-1, 1], Y up. Returns {ok, hit, nodeId?, id?}.
EMSCRIPTEN_KEEPALIVE
const char* saida_pick(float ndcX, float ndcY) {
    static std::string out;
    out = pickNode(ndcX, ndcY);
    return out.c_str();
}

// Editor graphics settings (UI-facing settings facade): {maxFps?, shadowsEnabled?}.
// maxFps caps the rAF loop by frame skipping (0 or out of [1,240] = uncapped);
// shadowsEnabled is the Renderer viewer-level switch. Viewer preferences only —
// nothing here mutates the scene document.
EMSCRIPTEN_KEEPALIVE
const char* saida_set_render_settings(const char* settingsJson) {
    static std::string out;
    try {
        const json s = json::parse(settingsJson ? settingsJson : "");
        if (s.contains("maxFps") && s["maxFps"].is_number()) {
            const double fps = s["maxFps"].get<double>();
            gApp.minFrameIntervalMs = (fps >= 1.0 && fps <= 240.0) ? 1000.0 / fps : 0.0;
            gApp.lastFrameMs = 0.0;
        }
        if (s.contains("shadowsEnabled") && s["shadowsEnabled"].is_boolean() && gApp.renderer)
            gApp.renderer->setShadowsEnabled(s["shadowsEnabled"].get<bool>());
        json r;
        r["ok"] = true;
        out = r.dump();
    } catch (const std::exception& e) {
        json r;
        r["ok"] = false;
        r["error"] = e.what();
        out = r.dump();
    }
    return out.c_str();
}

// Camera state for viewport tools (gizmos): world-space basis + projection
// parameters, enough to turn a screen-space drag into a world-space delta.
EMSCRIPTEN_KEEPALIVE
const char* saida_camera_state() {
    static std::string out;
    json r;
    r["ok"] = true;
    const auto vec = [](const glm::vec3& v) { return json::array({v.x, v.y, v.z}); };
    r["position"] = vec(gApp.camera.position);
    r["front"] = vec(gApp.camera.front());
    r["right"] = vec(gApp.camera.right());
    r["up"] = vec(gApp.camera.up());
    r["fovDegrees"] = gApp.camera.fovDegrees;
    r["aspect"] = float(kWidth) / float(kHeight);
    out = r.dump();
    return out.c_str();
}

} // extern "C"

int main() {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwCreateWindow(int(kWidth), int(kHeight), "SaidaEngine web", nullptr, nullptr);

    rhi::Device::requestAsync([](std::unique_ptr<rhi::Device> device) {
        if (!device) {
            gApp.startupError = "WebGPU unavailable";
            std::printf("saida-web: WebGPU unavailable\n");
            return;
        }
        try {
            gApp.device = std::move(device);
            gApp.surface =
                std::make_unique<rhi::Surface>(*gApp.device, "#canvas", kWidth, kHeight);
            initRuntime();
            if (!gApp.scene || !gApp.renderer) {
                gApp.startupError = "initial scene failed to load";
                return;
            }
            runtime::RoundTripContractReport contractReport;
            std::string contractError;
            if (!runtime::verifySnapshotRoundTripContract(
                    RuntimeTypeTarget::AuthoringWasm, contractReport, contractError))
                throw std::runtime_error("authoring Web runtime contract: " + contractError);
            std::printf("[CONTRACT] PASS authoringWasm nodes=%zu behaviours=%zu properties=%zu\n",
                        contractReport.nodes, contractReport.behaviours,
                        contractReport.reflectedProperties);
            // Also prove the concrete loaded scene is accepted by the durable
            // codec before the runtime publishes ready.
            (void)durableSceneSnapshotJson();
            gApp.ready = true;
            emscripten_set_main_loop(frame, 0, false);
        } catch (const std::exception& e) {
            gApp.startupError = e.what();
            std::printf("saida-web: startup failed: %s\n", e.what());
        }
    });

    emscripten_exit_with_live_runtime();
    return 0;
}
