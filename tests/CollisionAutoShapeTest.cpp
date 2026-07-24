// Regression test for the CollisionShape "Auto" mode.
//
// Bug: Auto resolution was one-shot. If it first ran while the mesh child's
// world transform was still identity (e.g. the editor wireframe touching it, or
// before Node::updateTransforms had propagated a freshly-loaded hierarchy), it
// froze a degenerate primitive — a unit-cube AABB resolves to a SPHERE of radius
// 0.5 — and never re-derived once the real (scaled) transform arrived. A floor
// authored as a (40,1,40)-scaled cube under a scale-1 StaticBody therefore got a
// tiny sphere collider, and a CharacterBody dropped onto it fell straight through.
//
// The fix caches the resolution against the mesh-in-body matrix and re-resolves
// when it changes (including the identity→scaled transition after a load). This
// exercises that path GPU-free via resolveAutoFrom (a unit-cube mesh AABB).

#include "physics/CollisionShapeNode.hpp"
#include "graphics/Mesh.hpp"  // Aabb

#include <cassert>
#include <cmath>
#include <cstdio>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

using namespace saida;

int main() {
    // A unit cube mesh: AABB from -0.5 to +0.5 on each axis.
    const Aabb unitCube{glm::vec3(-0.5f), glm::vec3(0.5f)};

    // 1) Stale identity transform — what the one-shot freeze used to capture.
    //    A unit cube is near-cubic, so Auto picks a sphere. This is the wrong
    //    result for a scaled floor, and the old code froze it permanently.
    CollisionShapeNode shape;
    shape.shapeType = CollisionShapeType::Auto;
    bool resolved = shape.resolveAutoFrom(unitCube, glm::mat4(1.0f));
    assert(resolved);
    assert(shape.resolvedType() == CollisionShapeType::Sphere);
    std::printf("identity transform -> %s (radius=%.2f)\n",
                toString(shape.resolvedType()), shape.radius);

    // 2) The real, scaled mesh-in-body transform arrives (mesh scaled 40,1,40 under
    //    a scale-1 body). The cache must notice the change and re-resolve to a box
    //    that actually covers the floor: half-extents (20, 0.5, 20).
    glm::mat4 scaled = glm::scale(glm::mat4(1.0f), glm::vec3(40.0f, 1.0f, 40.0f));
    resolved = shape.resolveAutoFrom(unitCube, scaled);
    assert(resolved);  // matrix changed -> must re-resolve, not keep the sphere
    assert(shape.resolvedType() == CollisionShapeType::Box);
    assert(std::fabs(shape.halfExtents.x - 20.0f) < 1e-3f);
    assert(std::fabs(shape.halfExtents.y - 0.5f) < 1e-3f);
    assert(std::fabs(shape.halfExtents.z - 20.0f) < 1e-3f);
    std::printf("scaled transform  -> %s half=(%.2f, %.2f, %.2f)\n",
                toString(shape.resolvedType()),
                shape.halfExtents.x, shape.halfExtents.y, shape.halfExtents.z);

    // 3) Stability: an unchanged transform must NOT re-resolve (moving/rotating the
    //    body leaves the mesh-in-body matrix invariant, so the shape stays put).
    resolved = shape.resolveAutoFrom(unitCube, scaled);
    assert(!resolved);
    assert(shape.resolvedType() == CollisionShapeType::Box);

    std::printf("PASS: Auto collider re-resolves to the correct box after load\n");
    return 0;
}
