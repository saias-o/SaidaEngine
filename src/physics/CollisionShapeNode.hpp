#pragma once

#include "scene/Node.hpp"

// Jolt config header first.
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <glm/glm.hpp>

namespace saida {

class Mesh;

enum class CollisionShapeType {
    Auto,        // pick a primitive from the mesh AABB (box/sphere/capsule)
    Box,
    Sphere,
    Capsule,
    ConvexHull,  // hull of the body's mesh; falls back to Box without CPU mesh data
    Mesh,        // triangle mesh, static only; falls back to Box without CPU mesh data
};

const char* toString(CollisionShapeType type);

// Resolved primitive (in the body's local frame) for editor visualization.
struct CollisionShapeViz {
    CollisionShapeType type = CollisionShapeType::Box;
    glm::vec3 halfExtents{0.5f};
    float radius = 0.5f;
    float height = 1.0f;
    int axis = 1;
    glm::vec3 offset{0.0f};
};

// A child of a physics body describing one collision primitive (Godot-style
// CollisionShape3D). Several shapes under one body form a compound.
class CollisionShapeNode : public Node {
public:
    CollisionShapeNode() : Node("CollisionShape") {}
    explicit CollisionShapeNode(std::string name) : Node(std::move(name)) {}

    const char* typeName() const override { return "CollisionShape"; }
    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;

    // Build a Jolt shape already positioned in the owning body's local frame.
    // `invBodyTR` = inverse(translation*rotation) of the body (scale excluded —
    // Jolt has no body scale, so scale is baked into the shape here). `bodyNode`
    // is searched for a mesh when in Auto mode. Returns null if no shape.
    JPH::Ref<JPH::Shape> buildShape(const glm::mat4& invBodyTR, Node& bodyNode);

    // Resolve the primitive (running Auto detection if needed) without building a
    // Jolt shape — used by the editor to draw the collider wireframe.
    CollisionShapeViz resolveViz(const glm::mat4& invBodyTR, Node& bodyNode);

    // Resolve Auto detection from the mesh expressed in the body's local frame
    // (`invBodyTR * meshWorld`). The result is cached against that source matrix:
    // moving/rotating the whole body leaves it invariant (so the shape stays
    // stable), but changing the mesh's scale/offset relative to the body — or the
    // identity→scaled transition right after a scene load — re-derives the shape.
    // No-op for non-Auto shapes. Returns true if detection just (re)ran —
    // the body must then rebuild its Jolt shape.
    bool ensureResolved(const glm::mat4& invBodyTR, Node& bodyNode);

    // True while the referenced mesh is still waiting for its geometry
    // (async .obj loading): buildShape produces nothing and the body is
    // deferred until the collision data exists.
    bool meshPending() const { return meshPending_; }
    // Re-arm Auto so the next ensureResolved detects again (editor "Recompute").
    void resetAuto() { autoResolved_ = false; }

    // Re-run Auto detection now (editor button); fills the manual params with the
    // detected values so the user can tweak from there.
    void autoDetectFrom(const class Aabb& bodyFrameBounds);

    // Core of Auto resolution, GPU-free for testing: derive the primitive from a
    // mesh AABB and the mesh-in-body matrix `toBody`, caching against that matrix.
    // Returns true if it (re-)resolved, false if the cached result was reused.
    // Detection input is the extent in the BODY's frame, because that is what
    // the detection reads and what the cache must key on: several meshes under
    // one body are unioned before they get here.
    bool resolveAutoFrom(const class Aabb& bodyBounds);
    // Single mesh: its own bounds seen through `toBody`. Convenience for the
    // one-mesh case and for tests that state the pair directly.
    bool resolveAutoFrom(const class Aabb& meshBounds, const glm::mat4& toBody);

    CollisionShapeType shapeType = CollisionShapeType::Auto;
    CollisionShapeType resolvedType() const { return resolved_; }

    // Manual parameters (used when shapeType != Auto; Auto fills them in too).
    glm::vec3 halfExtents{0.5f};  // Box
    float radius = 0.5f;          // Sphere / Capsule
    float height = 1.0f;          // Capsule total height (including the two caps)
    int axis = 1;                 // Capsule main axis: 0=X, 1=Y, 2=Z
    glm::vec3 offset{0.0f};       // shape center offset, in body-local space

private:
    CollisionShapeType resolved_ = CollisionShapeType::Box;
    bool autoResolved_ = false;        // true once Auto has detected a primitive
    bool meshPending_ = false;         // mesh proxy without geometry yet (async .obj)
    // Body-space extent the detection last used, as a pair rather than an Aabb
    // so this header need not pull in graphics/Mesh.hpp.
    glm::vec3 resolvedMin_{0.0f};
    glm::vec3 resolvedMax_{0.0f};
};

} // namespace saida
