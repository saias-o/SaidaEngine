#include "editor/ProfilerTriangleMetrics.hpp"

#include "core/Camera.hpp"
#include "graphics/Mesh.hpp"
#include "nodes/MeshNode.hpp"
#include "render/FrustumCulling.hpp"
#include "scene/Scene.hpp"

namespace saida {

MeshTriangleMetrics collectProfilerTriangleMetrics(const Scene& scene,
                                                   const Camera& camera) {
    MeshTriangleMetrics metrics;
    const Frustum frustum = camera.getFrustum();

    for (const MeshNode* node : scene.meshes()) {
        Mesh* mesh = node->mesh();
        if (!mesh || !node->material()) continue;

        if (node->hasLods()) {
            const int lod = node->activeLodIndex();
            mesh = node->meshForLod(lod);
            if (!mesh || !node->materialForLod(lod)) continue;
        }

        metrics.addIndexedMesh(
            mesh->allocation().indexCount,
            meshIntersectsFrustum(*mesh, node->worldTransform(), frustum));
    }

    return metrics;
}

} // namespace saida
