#pragma once

#include "graphics/Mesh.hpp"  // Vertex

#include <cstdint>
#include <string>
#include <vector>

namespace saida {

// One mesh to write into the GLB. Geometry only (the web asset pipeline's first
// concern); materials/skins can be layered on later.
struct ExportMesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;  // triangle list
    std::string name;
};

struct MeshoptExportOptions {
    bool quantize = true;
};

bool exportMeshoptGlb(const std::vector<ExportMesh>& meshes, const std::string& outPath,
                      const MeshoptExportOptions& options = {});

// Re-reads the geometry from a source .gltf/.glb (validated) or .obj — full
// fidelity comes from the file, not from a GPU readback.
bool collectExportMeshes(const std::string& sourcePath, std::vector<ExportMesh>& out,
                         std::string& error);

} // namespace saida
