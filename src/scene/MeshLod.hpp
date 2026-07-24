#pragma once

#include "graphics/Mesh.hpp"

#include <glm/mat4x4.hpp>
#include <vector>

namespace saida {

class Material;

// One level in a mesh LOD chain (MSFT_lod / MSFT_screencoverage).
struct MeshLodLevel {
    Mesh* mesh = nullptr;
    Material* material = nullptr;  // nullptr → fall back to the MeshNode's base material
    float minScreenCoverage = 0.0f; // minimum projected screen coverage to pick this LOD
};

// Projected bounding-sphere screen coverage in [0, 1] (matches MSFT_screencoverage semantics).
float computeScreenCoverage(const glm::mat4& world, const Aabb& localBounds,
                            const glm::mat4& view, const glm::mat4& proj);

// Pick the finest LOD whose minScreenCoverage threshold is met.
int selectLodIndex(float screenCoverage, const std::vector<MeshLodLevel>& lods);

// Convert MSFT_screencoverage array [1.0, t1, t2, ...] to per-level min thresholds.
std::vector<float> coverageThresholdsFromMsft(const std::vector<float>& msftCoverage, size_t lodCount);

} // namespace saida
