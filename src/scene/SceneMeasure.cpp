#include "scene/SceneMeasure.hpp"

#include "graphics/Mesh.hpp"
#include "scene/Node.hpp"
#include "scene/Scene.hpp"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace saida::measure {
namespace {

// Rounded so a report diffs cleanly between runs and reads as millimetres
// rather than as float noise. The engine's unit is the metre.
constexpr float kReportPrecision = 1000.0f;

float round3(float v) {
    const float r = std::round(v * kReportPrecision) / kReportPrecision;
    return r == 0.0f ? 0.0f : r;  // never report "-0"
}

nlohmann::json vec3Json(const glm::vec3& v) {
    return nlohmann::json::array({round3(v.x), round3(v.y), round3(v.z)});
}

nlohmann::json boundsJson(const WorldBounds& b) {
    nlohmann::json j;
    if (!b.valid) {
        j["drawsNothing"] = true;
        return j;
    }
    j["min"] = vec3Json(b.min);
    j["max"] = vec3Json(b.max);
    j["size"] = vec3Json(b.size());
    j["center"] = vec3Json(b.center());
    j["meshes"] = b.meshCount;
    j["triangles"] = b.triangles;
    return j;
}

uint64_t triangleCount(const Mesh& mesh) {
    return static_cast<uint64_t>(mesh.allocation().indexCount) / 3u;
}

void accumulate(const Node& node, WorldBounds& out) {
    if (!node.enabled()) return;
    out.expand(measureOwnMesh(node));
    for (const auto& child : node.children()) accumulate(*child, out);
}

Node* findByName(Node& node, const std::string& name) {
    if (node.name() == name) return &node;
    for (const auto& child : node.children())
        if (Node* found = findByName(*child, name)) return found;
    return nullptr;
}

Node* findInGroup(Node& node, const std::string& group) {
    if (node.isInGroup(group)) return &node;
    for (const auto& child : node.children())
        if (Node* found = findInGroup(*child, group)) return found;
    return nullptr;
}

// A report root is named the way the caller already names nodes elsewhere: a
// path, a node name, or a group. Resolving all three keeps the flag usable
// without first dumping the whole scene to learn the exact spelling.
Node* resolveRoot(Scene& scene, const std::string& spec) {
    if (spec.empty()) return &scene;
    if (Node* byPath = scene.findByPath(spec)) return byPath;
    if (Node* byName = findByName(scene, spec)) return byName;
    return findInGroup(scene, spec);
}

void describeNode(const Node& node, int depth, const ReportOptions& options,
                  nlohmann::json& out) {
    out["name"] = node.name();
    out["type"] = node.typeName();
    out["path"] = nodePath(node);
    if (!node.enabled()) out["enabled"] = false;
    if (!node.groups().empty()) out["groups"] = node.groups();

    nlohmann::json behaviours = nlohmann::json::array();
    for (const auto& behaviour : node.behaviours())
        if (const char* type = behaviour->typeName()) behaviours.push_back(type);
    if (!behaviours.empty()) out["behaviours"] = behaviours;

    const glm::mat4& world = node.worldTransform();
    glm::vec3 scale;
    glm::quat rotation;
    glm::vec3 translation;
    glm::vec3 skew;
    glm::vec4 perspective;
    nlohmann::json worldJson;
    if (glm::decompose(world, scale, rotation, translation, skew, perspective)) {
        worldJson["position"] = vec3Json(translation);
        worldJson["scale"] = vec3Json(scale);
        worldJson["yawDegrees"] = round3(glm::degrees(glm::yaw(rotation)));
    } else {
        worldJson["position"] = vec3Json(glm::vec3(world[3]));
    }
    out["world"] = worldJson;

    // The node's own drawn size, separate from its subtree's: a chassis that is
    // right on its own and wrong beside its wheels is exactly the case this
    // report exists for, and one combined number hides it.
    const WorldBounds own = measureOwnMesh(node);
    if (own.valid) out["mesh"] = boundsJson(own);

    const WorldBounds subtree = measureSubtree(node);
    out["bounds"] = boundsJson(subtree);

    const bool atDepthLimit = options.maxDepth > 0 && depth >= options.maxDepth;
    if (node.children().empty() || atDepthLimit) {
        if (atDepthLimit && !node.children().empty())
            out["childrenNotListed"] = static_cast<int>(node.children().size());
        return;
    }

    nlohmann::json children = nlohmann::json::array();
    for (const auto& child : node.children()) {
        nlohmann::json childJson;
        describeNode(*child, depth + 1, options, childJson);
        children.push_back(std::move(childJson));
    }
    out["children"] = std::move(children);
}

}  // namespace

void WorldBounds::expand(const WorldBounds& other) {
    if (!other.valid) return;
    if (!valid) {
        min = other.min;
        max = other.max;
        valid = true;
    } else {
        min = glm::min(min, other.min);
        max = glm::max(max, other.max);
    }
    meshCount += other.meshCount;
    triangles += other.triangles;
}

WorldBounds transformedBounds(const Aabb& local, const glm::mat4& world) {
    WorldBounds out;
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 point{(corner & 1) ? local.max.x : local.min.x,
                              (corner & 2) ? local.max.y : local.min.y,
                              (corner & 4) ? local.max.z : local.min.z};
        const glm::vec3 world_point = glm::vec3(world * glm::vec4(point, 1.0f));
        lo = glm::min(lo, world_point);
        hi = glm::max(hi, world_point);
    }
    out.min = lo;
    out.max = hi;
    out.valid = true;
    return out;
}

WorldBounds measureOwnMesh(const Node& node) {
    const Mesh* mesh = node.mesh();
    if (!mesh || !mesh->loaded()) return {};
    WorldBounds out = transformedBounds(mesh->bounds(), node.worldTransform());
    out.meshCount = 1;
    out.triangles = triangleCount(*mesh);
    return out;
}

WorldBounds measureSubtree(const Node& root) {
    WorldBounds out;
    accumulate(root, out);
    return out;
}

std::string nodePath(const Node& node) {
    std::string path = node.name();
    for (const Node* parent = node.parent(); parent; parent = parent->parent())
        path = parent->name() + "/" + path;
    return "/" + path;
}

nlohmann::json describeScene(Scene& scene, const ReportOptions& options, std::string& error) {
    scene.refreshHierarchy();
    Node* root = resolveRoot(scene, options.root);
    if (!root) {
        error = "no node, path or group named '" + options.root + "'";
        return nlohmann::json::object();
    }

    nlohmann::json report;
    report["schema"] = "saida.scene-measure";
    report["version"] = 1;
    report["unit"] = "metre";

    const WorldBounds total = measureSubtree(*root);
    report["totals"] = boundsJson(total);

    nlohmann::json rootJson;
    describeNode(*root, 0, options, rootJson);
    report["root"] = std::move(rootJson);
    return report;
}

}  // namespace saida::measure
