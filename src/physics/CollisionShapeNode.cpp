#include "physics/CollisionShapeNode.hpp"

#include "physics/JoltGlue.hpp"
#include "graphics/Mesh.hpp"
#include "core/Log.hpp"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>

#include <glm/gtc/quaternion.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>

namespace saida {

const char* toString(CollisionShapeType type) {
    switch (type) {
        case CollisionShapeType::Auto: return "Auto";
        case CollisionShapeType::Box: return "Box";
        case CollisionShapeType::Sphere: return "Sphere";
        case CollisionShapeType::Capsule: return "Capsule";
        case CollisionShapeType::ConvexHull: return "ConvexHull";
        case CollisionShapeType::Mesh: return "Mesh";
    }
    return "Unknown";
}

namespace {

// Smallest half-extent / radius we let through so Jolt's convex radius is valid.
constexpr float kMinHalf = 0.02f;

// Find the first drawable mesh in a subtree, returning it and its world matrix.
Mesh* findMesh(Node& n, glm::mat4& outWorld) {
    if (Mesh* m = n.mesh()) {
        outWorld = n.worldTransform();
        return m;
    }
    for (const auto& c : n.children()) {
        if (Mesh* m = findMesh(*c, outWorld)) return m;
    }
    return nullptr;
}

// Transform a mesh-local AABB by `m` and return the enclosing axis-aligned box.
Aabb transformAabb(const Aabb& box, const glm::mat4& m) {
    glm::vec3 mn(std::numeric_limits<float>::max());
    glm::vec3 mx(std::numeric_limits<float>::lowest());
    for (int i = 0; i < 8; ++i) {
        glm::vec3 corner((i & 1) ? box.max.x : box.min.x,
                         (i & 2) ? box.max.y : box.min.y,
                         (i & 4) ? box.max.z : box.min.z);
        glm::vec3 p = glm::vec3(m * glm::vec4(corner, 1.0f));
        mn = glm::min(mn, p);
        mx = glm::max(mx, p);
    }
    return {mn, mx};
}

// Rotation that maps Jolt's +Y capsule axis onto the requested body axis.
JPH::Quat capsuleAxisRotation(int axis) {
    switch (axis) {
        case 0: return JPH::Quat::sRotation(JPH::Vec3(0, 0, 1), -JPH::JPH_PI * 0.5f);  // Y -> X
        case 2: return JPH::Quat::sRotation(JPH::Vec3(1, 0, 0), JPH::JPH_PI * 0.5f);   // Y -> Z
        default: return JPH::Quat::sIdentity();
    }
}

// Convex hull of the body's mesh, baked into the body's local (unscaled) frame.
// Dynamic-capable. Null if there is no CPU mesh data.
JPH::Ref<JPH::Shape> buildConvexHull(Node& bodyNode, const glm::mat4& invBodyTR) {
    using namespace JPH;
    glm::mat4 meshWorld(1.0f);
    Mesh* m = findMesh(bodyNode, meshWorld);
    if (!m || m->collisionVertices().empty()) return Ref<Shape>();

    glm::mat4 toBody = invBodyTR * meshWorld;
    Array<Vec3> pts;
    pts.reserve(m->collisionVertices().size());
    for (const glm::vec3& p : m->collisionVertices()) {
        glm::vec3 bp = glm::vec3(toBody * glm::vec4(p, 1.0f));
        pts.push_back(Vec3(bp.x, bp.y, bp.z));
    }
    ShapeSettings::ShapeResult r = ConvexHullShapeSettings(pts).Create();
    if (!r.IsValid()) {
        Log::warn("CollisionShape: convex hull failed: ", r.GetError().c_str());
        return Ref<Shape>();
    }
    return r.Get();
}

// Exact triangle mesh of the body's mesh. STATIC bodies only (no inertia).
JPH::Ref<JPH::Shape> buildTriangleMesh(Node& bodyNode, const glm::mat4& invBodyTR) {
    using namespace JPH;
    glm::mat4 meshWorld(1.0f);
    Mesh* m = findMesh(bodyNode, meshWorld);
    if (!m || m->collisionVertices().empty() || m->collisionIndices().size() < 3) return Ref<Shape>();

    glm::mat4 toBody = invBodyTR * meshWorld;
    VertexList verts;
    verts.reserve(m->collisionVertices().size());
    for (const glm::vec3& p : m->collisionVertices()) {
        glm::vec3 bp = glm::vec3(toBody * glm::vec4(p, 1.0f));
        verts.push_back(Float3(bp.x, bp.y, bp.z));
    }
    IndexedTriangleList tris;
    const std::vector<uint32_t>& idx = m->collisionIndices();
    tris.reserve(idx.size() / 3);
    for (size_t i = 0; i + 2 < idx.size(); i += 3)
        tris.push_back(IndexedTriangle(idx[i], idx[i + 1], idx[i + 2], 0));

    ShapeSettings::ShapeResult r = MeshShapeSettings(verts, tris).Create();
    if (!r.IsValid()) {
        Log::warn("CollisionShape: triangle mesh failed: ", r.GetError().c_str());
        return Ref<Shape>();
    }
    return r.Get();
}

JPH::Ref<JPH::Shape> buildBox(const glm::vec3& halfExtents) {
    using namespace JPH;
    glm::vec3 he = glm::max(halfExtents, glm::vec3(kMinHalf));
    ShapeSettings::ShapeResult r = BoxShapeSettings(toJolt(he)).Create();
    return r.IsValid() ? r.Get() : Ref<Shape>();
}

} // namespace

void CollisionShapeNode::autoDetectFrom(const Aabb& b) {
    glm::vec3 e = glm::max(b.extent(), glm::vec3(2.0f * kMinHalf));
    offset = b.center();

    // Sort the three extents, remembering which axis is the longest.
    std::array<std::pair<float, int>, 3> ax{{{e.x, 0}, {e.y, 1}, {e.z, 2}}};
    std::sort(ax.begin(), ax.end(), [](auto& a, auto& b2) { return a.first < b2.first; });
    float e0 = ax[0].first, e1 = ax[1].first, e2 = ax[2].first;  // e0 <= e1 <= e2
    int longAxis = ax[2].second;

    if (e2 / e0 < 1.25f) {
        // Near-cubic / blobby → sphere (e.g. a stocky mob).
        resolved_ = CollisionShapeType::Sphere;
        radius = e2 * 0.5f;
    } else if (e1 / e0 < 1.4f && e2 / e1 > 1.5f) {
        // Two similar small axes + one long axis → capsule (e.g. a humanoid).
        resolved_ = CollisionShapeType::Capsule;
        radius = (e0 + e1) * 0.25f;
        height = e2;
        axis = longAxis;
    } else {
        resolved_ = CollisionShapeType::Box;
        halfExtents = e * 0.5f;
    }
}

bool CollisionShapeNode::ensureResolved(const glm::mat4& invBodyTR, Node& bodyNode) {
    // A mesh proxy that's still empty (async .obj loading) provides neither
    // bounds nor collision data: defer both resolution AND the body
    // (meshPending_) until the geometry arrives. The pending->loaded
    // transition re-resolves and returns true -> the body rebuilds.
    {
        glm::mat4 ignored(1.0f);
        Mesh* m = findMesh(bodyNode, ignored);
        const bool pending = m && !m->loaded();
        const bool becameReady = meshPending_ && !pending;
        meshPending_ = pending;
        if (pending) return false;
        if (becameReady && shapeType != CollisionShapeType::Auto) {
            resolved_ = shapeType;
            return true;  // ConvexHull/Mesh re-read the collision data
        }
    }

    if (shapeType != CollisionShapeType::Auto) {
        resolved_ = shapeType;
        return false;
    }

    // Derive the primitive from the mesh AABB expressed in the body's local
    // (unscaled) frame; this bakes mesh-vs-body scale into the shape. The source
    // matrix is invariant under moving/rotating the body, so a stable shape only
    // re-resolves when the mesh's scale/offset relative to the body actually
    // changes — including the identity→scaled transition right after a load.
    glm::mat4 meshWorld(1.0f);
    if (Mesh* m = findMesh(bodyNode, meshWorld)) {
        return resolveAutoFrom(m->bounds(), invBodyTR * meshWorld);
    }
    if (!autoResolved_) {
        resolved_ = CollisionShapeType::Box;
        halfExtents = glm::vec3(0.5f);
        autoResolved_ = true;
    }
    return false;
}

bool CollisionShapeNode::resolveAutoFrom(const Aabb& meshBounds, const glm::mat4& toBody) {
    if (autoResolved_ && toBody == resolvedFrom_) return false;  // unchanged → keep result
    autoDetectFrom(transformAabb(meshBounds, toBody));
    autoResolved_ = true;
    resolvedFrom_ = toBody;
    return true;
}

CollisionShapeViz CollisionShapeNode::resolveViz(const glm::mat4& invBodyTR, Node& bodyNode) {
    ensureResolved(invBodyTR, bodyNode);
    return {resolved_, halfExtents, radius, height, axis, offset};
}

JPH::Ref<JPH::Shape> CollisionShapeNode::buildShape(const glm::mat4& invBodyTR, Node& bodyNode) {
    using namespace JPH;

    ensureResolved(invBodyTR, bodyNode);
    // Geometry not loaded yet: no shape (rather than a wrong box);
    // the body will be built once the mesh arrives (resolveAutoShapes -> dirty).
    if (meshPending_) return Ref<Shape>();

    // Build the primitive.
    Ref<Shape> prim;
    Quat localRot = Quat::sIdentity();
    switch (resolved_) {
        case CollisionShapeType::Sphere: {
            ShapeSettings::ShapeResult r = SphereShapeSettings(std::max(radius, kMinHalf)).Create();
            if (r.IsValid()) prim = r.Get();
            break;
        }
        case CollisionShapeType::Capsule: {
            float rad = std::max(radius, kMinHalf);
            float halfCyl = std::max(0.0f, height * 0.5f - rad);
            ShapeSettings::ShapeResult r = CapsuleShapeSettings(halfCyl, rad).Create();
            if (r.IsValid()) {
                prim = r.Get();
                localRot = capsuleAxisRotation(axis);
            }
            break;
        }
        case CollisionShapeType::ConvexHull:
            // Already in body-local space → return directly (no offset wrapper).
            if (Ref<Shape> s = buildConvexHull(bodyNode, invBodyTR)) return s;
            Log::warn("CollisionShape '", name(), "': convex hull unavailable, using box");
            prim = buildBox(halfExtents);
            break;
        case CollisionShapeType::Mesh:
            if (Ref<Shape> s = buildTriangleMesh(bodyNode, invBodyTR)) return s;
            Log::warn("CollisionShape '", name(), "': triangle mesh unavailable, using box");
            prim = buildBox(halfExtents);
            break;
        case CollisionShapeType::Box:
        default:
            prim = buildBox(halfExtents);
            break;
    }
    if (prim == nullptr) return Ref<Shape>();

    // Position the primitive at its offset (and capsule axis rotation) within the body.
    if (offset != glm::vec3(0.0f) || localRot != Quat::sIdentity()) {
        ShapeSettings::ShapeResult r =
            RotatedTranslatedShapeSettings(toJolt(offset), localRot, prim.GetPtr()).Create();
        if (r.IsValid()) return r.Get();
    }
    return prim;
}

void CollisionShapeNode::serialize(nlohmann::json& j, ResourceManager& resources) const {
    Node::serialize(j, resources);
    j["shapeType"] = static_cast<int>(shapeType);
    j["halfExtents"] = {halfExtents.x, halfExtents.y, halfExtents.z};
    j["radius"] = radius;
    j["height"] = height;
    j["axis"] = axis;
    j["offset"] = {offset.x, offset.y, offset.z};
}

void CollisionShapeNode::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    Node::deserialize(j, resources);
    if (j.contains("shapeType")) shapeType = static_cast<CollisionShapeType>(j["shapeType"].get<int>());
    if (j.contains("halfExtents") && j["halfExtents"].is_array() && j["halfExtents"].size() == 3)
        halfExtents = {j["halfExtents"][0], j["halfExtents"][1], j["halfExtents"][2]};
    if (j.contains("radius")) radius = j["radius"].get<float>();
    if (j.contains("height")) height = j["height"].get<float>();
    if (j.contains("axis")) axis = j["axis"].get<int>();
    if (j.contains("offset") && j["offset"].is_array() && j["offset"].size() == 3)
        offset = {j["offset"][0], j["offset"][1], j["offset"][2]};
}

} // namespace saida
