#pragma once

#include <cstdint>

namespace saida {

inline constexpr const char* kProfilerFrustumTriangles =
    "Renderer/FrustumTriangles";
inline constexpr const char* kProfilerSceneTriangles =
    "Scene/TotalTriangles";

// Pure accumulator used by the editor-only collector and its contract test.
// Counts indexed geometry once per active MeshNode instance, independently of
// how many render passes consume it. Shadows, post-processing, particles and UI
// deliberately do not inflate these values.
struct MeshTriangleMetrics {
    uint64_t frustumTriangles = 0;
    uint64_t sceneTriangles = 0;

    void addIndexedMesh(uint32_t indexCount, bool inFrustum) {
        const uint64_t triangles = indexCount / 3u;
        sceneTriangles += triangles;
        if (inFrustum) frustumTriangles += triangles;
    }
};

} // namespace saida
