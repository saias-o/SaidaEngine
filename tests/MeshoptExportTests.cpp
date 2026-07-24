// Round-trips geometry through the meshopt GLB exporter and the GLTFLoader's
// decode path, with no GPU: build a mesh -> exportMeshoptGlb -> re-parse with
// cgltf -> decodeMeshoptBuffers -> read accessors back -> compare geometry.
// Proves the EXT_meshopt_compression encode+decode loop end to end.

#include "cli/MeshoptGlbExporter.hpp"
#include "scene/GltfMeshopt.hpp"
#include "graphics/Mesh.hpp"

#include <cgltf.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <tuple>
#include <vector>

namespace {

void require(bool condition) {
    if (!condition) std::abort();
}

std::filesystem::path testFile() {
    auto root = std::filesystem::temp_directory_path() / "SaidaEngineMeshoptTests";
    std::filesystem::create_directories(root);
    return root / "cube.glb";
}

// A unit cube: 8 corner vertices, 12 triangles (36 indices). Shared corners make
// the vertex-cache / vertex-fetch optimization meaningful.
saida::ExportMesh makeCube() {
    saida::ExportMesh m;
    m.name = "Cube";
    const std::array<glm::vec3, 8> corners = {{
        {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
        {-1, -1,  1}, {1, -1,  1}, {1, 1,  1}, {-1, 1,  1},
    }};
    for (size_t i = 0; i < corners.size(); ++i) {
        saida::Vertex v{};
        v.pos = corners[i];
        v.normal = glm::normalize(corners[i]);
        v.color = glm::vec3(1.0f);
        v.texCoord = glm::vec2(float(i) * 0.1f, float(i) * 0.2f);
        v.lightmapUV = v.texCoord;
        v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        m.vertices.push_back(v);
    }
    m.indices = {
        0, 1, 2, 0, 2, 3,  // back
        4, 6, 5, 4, 7, 6,  // front
        0, 4, 5, 0, 5, 1,  // bottom
        3, 2, 6, 3, 6, 7,  // top
        0, 3, 7, 0, 7, 4,  // left
        1, 5, 6, 1, 6, 2,  // right
    };
    return m;
}

// Triangle "soup" canonicalized so it can be compared regardless of how the
// optimizer reordered vertices/indices: each triangle's 3 positions are sorted,
// then all triangles are sorted. Geometry equality <=> identical soups.
using Tri = std::array<float, 9>;
std::vector<Tri> triangleSoup(const std::vector<glm::vec3>& pos,
                              const std::vector<uint32_t>& idx) {
    std::vector<Tri> soup;
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        std::array<glm::vec3, 3> p = {pos[idx[t]], pos[idx[t + 1]], pos[idx[t + 2]]};
        std::sort(p.begin(), p.end(), [](const glm::vec3& a, const glm::vec3& b) {
            return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
        });
        soup.push_back({p[0].x, p[0].y, p[0].z, p[1].x, p[1].y, p[1].z, p[2].x, p[2].y, p[2].z});
    }
    std::sort(soup.begin(), soup.end());
    return soup;
}

void testCubeRoundTrip() {
    const saida::ExportMesh cube = makeCube();
    const auto path = testFile();
    saida::MeshoptExportOptions lossless;
    lossless.quantize = false;
    require(saida::exportMeshoptGlb({cube}, path.string(), lossless));

    // Re-parse the GLB we just wrote.
    cgltf_options opts = {};
    cgltf_data* data = nullptr;
    require(cgltf_parse_file(&opts, path.string().c_str(), &data) == cgltf_result_success);
    require(cgltf_load_buffers(&opts, data, path.string().c_str()) == cgltf_result_success);

    // The buffer views must actually be meshopt-compressed.
    bool sawCompression = false;
    for (size_t i = 0; i < data->buffer_views_count; ++i)
        sawCompression |= data->buffer_views[i].has_meshopt_compression != 0;
    require(sawCompression);

    require(saida::decodeMeshoptBuffers(data));

    require(data->meshes_count == 1);
    require(data->meshes[0].primitives_count == 1);
    cgltf_primitive& prim = data->meshes[0].primitives[0];

    cgltf_accessor* posAcc = nullptr;
    for (size_t a = 0; a < prim.attributes_count; ++a)
        if (prim.attributes[a].type == cgltf_attribute_type_position)
            posAcc = prim.attributes[a].data;
    require(posAcc != nullptr);
    require(prim.indices != nullptr);

    std::vector<glm::vec3> positions(posAcc->count);
    for (size_t v = 0; v < posAcc->count; ++v)
        cgltf_accessor_read_float(posAcc, v, glm::value_ptr(positions[v]), 3);

    std::vector<uint32_t> indices(prim.indices->count);
    for (size_t i = 0; i < prim.indices->count; ++i)
        indices[i] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i));

    // Same triangle count as the source.
    require(indices.size() == cube.indices.size());

    // Geometry is preserved exactly (meshopt vertex codec is lossless).
    std::vector<glm::vec3> srcPos;
    for (const saida::Vertex& v : cube.vertices) srcPos.push_back(v.pos);
    require(triangleSoup(srcPos, cube.indices) == triangleSoup(positions, indices));

    cgltf_free(data);
}

void testQuantizedRoundTrip() {
    const saida::ExportMesh cube = makeCube();
    const auto path = testFile();
    saida::MeshoptExportOptions options;
    options.quantize = true;
    require(saida::exportMeshoptGlb({cube}, path.string(), options));

    cgltf_options opts = {};
    cgltf_data* data = nullptr;
    require(cgltf_parse_file(&opts, path.string().c_str(), &data) == cgltf_result_success);
    require(cgltf_load_buffers(&opts, data, path.string().c_str()) == cgltf_result_success);
    require(saida::decodeMeshoptBuffers(data));

    cgltf_primitive& prim = data->meshes[0].primitives[0];
    cgltf_accessor* posAcc = nullptr;
    cgltf_accessor* nrmAcc = nullptr;
    for (size_t a = 0; a < prim.attributes_count; ++a) {
        if (prim.attributes[a].type == cgltf_attribute_type_position)
            posAcc = prim.attributes[a].data;
        if (prim.attributes[a].type == cgltf_attribute_type_normal)
            nrmAcc = prim.attributes[a].data;
    }
    require(posAcc && nrmAcc);
    // Normals are int8-normalized (KHR_mesh_quantization)...
    require(nrmAcc->component_type == cgltf_component_type_r_8);
    require(nrmAcc->normalized != 0);
    // ...positions stay exact floats...
    require(posAcc->component_type == cgltf_component_type_r_32f);
    std::vector<glm::vec3> positions(posAcc->count);
    for (size_t v = 0; v < posAcc->count; ++v)
        cgltf_accessor_read_float(posAcc, v, glm::value_ptr(positions[v]), 3);
    std::vector<uint32_t> indices(prim.indices->count);
    for (size_t i = 0; i < prim.indices->count; ++i)
        indices[i] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i));
    std::vector<glm::vec3> srcPos;
    for (const saida::Vertex& v : cube.vertices) srcPos.push_back(v.pos);
    require(triangleSoup(srcPos, cube.indices) == triangleSoup(positions, indices));
    // ...and normals decode back within int8 precision (axis-aligned cube: exact).
    for (size_t v = 0; v < nrmAcc->count; ++v) {
        glm::vec3 n;
        cgltf_accessor_read_float(nrmAcc, v, glm::value_ptr(n), 3);
        require(std::abs(glm::length(n) - 1.0f) < 0.02f);
    }

    cgltf_free(data);
}

} // namespace

// collectExportMeshes (source -> ExportMesh) then export: the full path of the
// editor's "Export meshopt GLB" button, without a GPU.
void testCollectFromObjAndExport() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "saida_meshopt_collect";
    fs::create_directories(dir);
    const fs::path objPath = dir / "tri.obj";
    {
        std::ofstream out(objPath);
        out << "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 1\n"
               "f 1/1/1 2/2/1 3/3/1\n";
    }

    std::vector<saida::ExportMesh> meshes;
    std::string error;
    require(saida::collectExportMeshes(objPath.string(), meshes, error));
    require(meshes.size() == 1);
    require(meshes[0].vertices.size() == 3 && meshes[0].indices.size() == 3);

    const fs::path glbPath = dir / "tri.meshopt.glb";
    require(saida::exportMeshoptGlb(meshes, glbPath.string()));

    // The produced GLB must be re-readable by collectExportMeshes (cgltf +
    // meshopt decode), looping collect -> export -> collect.
    std::vector<saida::ExportMesh> reread;
    require(saida::collectExportMeshes(glbPath.string(), reread, error));
    require(reread.size() == 1 && reread[0].indices.size() == 3);

    require(!saida::collectExportMeshes((dir / "missing.gltf").string(), meshes, error));
    fs::remove_all(dir);
}

int main() {
    testCubeRoundTrip();
    testQuantizedRoundTrip();
    testCollectFromObjAndExport();
    return 0;
}
