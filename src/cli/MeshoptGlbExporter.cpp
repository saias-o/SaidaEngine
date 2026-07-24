#include "cli/MeshoptGlbExporter.hpp"
#include "core/Log.hpp"
#include "scene/GltfMeshopt.hpp"

#include <meshoptimizer.h>
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>

#include <cgltf.h>

#include <cstddef>  // offsetof
#include <filesystem>
#include <cstring>
#include <fstream>
#include <limits>

namespace saida {
namespace {

using json = nlohmann::json;

void append(std::vector<uint8_t>& buf, const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    buf.insert(buf.end(), p, p + size);
}
void padTo4(std::vector<uint8_t>& buf, uint8_t pad) {
    while (buf.size() % 4 != 0) buf.push_back(pad);
}
void writeU32(std::ofstream& os, uint32_t v) {
    const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
    os.write(reinterpret_cast<const char*>(b), 4);
}

// glTF componentType constants.
constexpr int kByte = 5120;
constexpr int kUnsignedShort = 5123;
constexpr int kUnsignedInt = 5125;
constexpr int kFloat = 5126;

// Per-mesh packed layout description in quantized mode.
struct QuantLayout {
    bool uv0Quantized = false;
    bool uv1Quantized = false;
    size_t offNormal = 12;   // after float3 position
    size_t offTangent = 16;
    size_t offUv0 = 20;
    size_t offUv1 = 0;       // computed
    size_t stride = 0;       // computed
};

bool uvFitsUnorm(const std::vector<Vertex>& verts, glm::vec2 Vertex::*member) {
    for (const Vertex& v : verts) {
        const glm::vec2& uv = v.*member;
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) return false;
    }
    return true;
}

QuantLayout planQuantLayout(const std::vector<Vertex>& verts) {
    QuantLayout q;
    q.uv0Quantized = uvFitsUnorm(verts, &Vertex::texCoord);
    q.uv1Quantized = uvFitsUnorm(verts, &Vertex::lightmapUV);
    q.offUv1 = q.offUv0 + (q.uv0Quantized ? 4 : 8);
    q.stride = q.offUv1 + (q.uv1Quantized ? 4 : 8);
    // Keep the stride a multiple of 4 (accessor alignment + encoder friendliness).
    q.stride = (q.stride + 3) & ~size_t(3);
    return q;
}

std::vector<uint8_t> packQuantized(const std::vector<Vertex>& verts, const QuantLayout& q) {
    std::vector<uint8_t> packed(verts.size() * q.stride, 0);
    for (size_t i = 0; i < verts.size(); ++i) {
        uint8_t* out = packed.data() + i * q.stride;
        const Vertex& v = verts[i];
        std::memcpy(out, &v.pos, 12);

        int8_t n[4] = {int8_t(meshopt_quantizeSnorm(v.normal.x, 8)),
                       int8_t(meshopt_quantizeSnorm(v.normal.y, 8)),
                       int8_t(meshopt_quantizeSnorm(v.normal.z, 8)), 0};
        std::memcpy(out + q.offNormal, n, 4);

        int8_t t[4] = {int8_t(meshopt_quantizeSnorm(v.tangent.x, 8)),
                       int8_t(meshopt_quantizeSnorm(v.tangent.y, 8)),
                       int8_t(meshopt_quantizeSnorm(v.tangent.z, 8)),
                       int8_t(meshopt_quantizeSnorm(v.tangent.w, 8))};
        std::memcpy(out + q.offTangent, t, 4);

        if (q.uv0Quantized) {
            uint16_t uv[2] = {uint16_t(meshopt_quantizeUnorm(v.texCoord.x, 16)),
                              uint16_t(meshopt_quantizeUnorm(v.texCoord.y, 16))};
            std::memcpy(out + q.offUv0, uv, 4);
        } else {
            std::memcpy(out + q.offUv0, &v.texCoord, 8);
        }
        if (q.uv1Quantized) {
            uint16_t uv[2] = {uint16_t(meshopt_quantizeUnorm(v.lightmapUV.x, 16)),
                              uint16_t(meshopt_quantizeUnorm(v.lightmapUV.y, 16))};
            std::memcpy(out + q.offUv1, uv, 4);
        } else {
            std::memcpy(out + q.offUv1, &v.lightmapUV, 8);
        }
    }
    return packed;
}

} // namespace

bool exportMeshoptGlb(const std::vector<ExportMesh>& meshes, const std::string& outPath,
                      const MeshoptExportOptions& options) {
    std::vector<uint8_t> bin;  // buffer 0: compressed data (lives in the BIN chunk)
    json bufferViews = json::array();
    json accessors = json::array();
    json jmeshes = json::array();
    json nodes = json::array();
    json sceneNodes = json::array();

    size_t fallbackOffset = 0;  // running offset into the data-less fallback buffer 1

    for (const ExportMesh& m : meshes) {
        if (m.vertices.empty() || m.indices.empty()) {
            Log::warn("MeshoptGlbExporter: skipping mesh '", m.name, "' (no geometry)");
            continue;
        }
        const size_t vertexCount = m.vertices.size();
        const size_t indexCount = m.indices.size();

        // 1. Optimize: vertex-cache reorder of indices, then vertex-fetch reorder
        //    of vertices (rewrites the indices to match the new vertex order).
        std::vector<uint32_t> indices(indexCount);
        meshopt_optimizeVertexCache(indices.data(), m.indices.data(), indexCount, vertexCount);

        std::vector<Vertex> verts(vertexCount);
        const size_t uniqueCount = meshopt_optimizeVertexFetch(
            verts.data(), indices.data(), indexCount, m.vertices.data(), vertexCount,
            sizeof(Vertex));
        verts.resize(uniqueCount);

        // 2. Optionally quantize into a packed layout, then entropy-code.
        QuantLayout q;
        const uint8_t* vertexBytes = reinterpret_cast<const uint8_t*>(verts.data());
        size_t stride = sizeof(Vertex);
        std::vector<uint8_t> packed;
        if (options.quantize) {
            q = planQuantLayout(verts);
            packed = packQuantized(verts, q);
            vertexBytes = packed.data();
            stride = q.stride;
        }

        std::vector<uint8_t> vbuf(meshopt_encodeVertexBufferBound(uniqueCount, stride));
        vbuf.resize(meshopt_encodeVertexBuffer(vbuf.data(), vbuf.size(), vertexBytes,
                                               uniqueCount, stride));

        std::vector<uint8_t> ibuf(meshopt_encodeIndexBufferBound(indexCount, uniqueCount));
        ibuf.resize(meshopt_encodeIndexBuffer(ibuf.data(), ibuf.size(), indices.data(), indexCount));

        // 3. Lay the compressed bytes into the BIN buffer (4-byte aligned).
        const size_t vCompOffset = bin.size();
        append(bin, vbuf.data(), vbuf.size());
        padTo4(bin, 0);
        const size_t iCompOffset = bin.size();
        append(bin, ibuf.data(), ibuf.size());
        padTo4(bin, 0);

        // Fallback (uncompressed) layout in buffer 1 — never read (decode overrides
        // view->data), kept only so the file is spec-consistent.
        const size_t vUncompLen = uniqueCount * stride;
        const size_t iUncompLen = indexCount * sizeof(uint32_t);
        const size_t vFallbackOffset = fallbackOffset; fallbackOffset += (vUncompLen + 3) & ~size_t(3);
        const size_t iFallbackOffset = fallbackOffset; fallbackOffset += (iUncompLen + 3) & ~size_t(3);

        const int vViewIdx = int(bufferViews.size());
        bufferViews.push_back({
            {"buffer", 1}, {"byteOffset", vFallbackOffset}, {"byteLength", vUncompLen},
            {"byteStride", stride}, {"target", 34962 /*ARRAY_BUFFER*/},
            {"extensions", {{"EXT_meshopt_compression", {
                {"buffer", 0}, {"byteOffset", vCompOffset}, {"byteLength", vbuf.size()},
                {"byteStride", stride}, {"count", uniqueCount}, {"mode", "ATTRIBUTES"}}}}},
        });
        const int iViewIdx = int(bufferViews.size());
        bufferViews.push_back({
            {"buffer", 1}, {"byteOffset", iFallbackOffset}, {"byteLength", iUncompLen},
            {"target", 34963 /*ELEMENT_ARRAY_BUFFER*/},
            {"extensions", {{"EXT_meshopt_compression", {
                {"buffer", 0}, {"byteOffset", iCompOffset}, {"byteLength", ibuf.size()},
                {"byteStride", sizeof(uint32_t)}, {"count", indexCount}, {"mode", "TRIANGLES"}}}}},
        });

        // 4. Accessors. POSITION carries min/max (required by glTF).
        glm::vec3 mn(std::numeric_limits<float>::max());
        glm::vec3 mx(-std::numeric_limits<float>::max());
        for (const Vertex& v : verts) { mn = glm::min(mn, v.pos); mx = glm::max(mx, v.pos); }

        auto accessor = [&](size_t byteOffset, int componentType, bool normalized,
                            const char* type) {
            const int idx = int(accessors.size());
            json a = {{"bufferView", vViewIdx}, {"byteOffset", byteOffset},
                      {"componentType", componentType}, {"count", uniqueCount}, {"type", type}};
            if (normalized) a["normalized"] = true;
            accessors.push_back(a);
            return idx;
        };

        const int aPos = int(accessors.size());
        accessors.push_back({
            {"bufferView", vViewIdx}, {"byteOffset", 0},
            {"componentType", kFloat}, {"count", uniqueCount}, {"type", "VEC3"},
            {"min", {mn.x, mn.y, mn.z}}, {"max", {mx.x, mx.y, mx.z}}});

        int aNrm, aUv0, aUv1, aTan;
        if (options.quantize) {
            aNrm = accessor(q.offNormal, kByte, true, "VEC3");
            aTan = accessor(q.offTangent, kByte, true, "VEC4");
            aUv0 = q.uv0Quantized ? accessor(q.offUv0, kUnsignedShort, true, "VEC2")
                                  : accessor(q.offUv0, kFloat, false, "VEC2");
            aUv1 = q.uv1Quantized ? accessor(q.offUv1, kUnsignedShort, true, "VEC2")
                                  : accessor(q.offUv1, kFloat, false, "VEC2");
        } else {
            aNrm = accessor(offsetof(Vertex, normal), kFloat, false, "VEC3");
            aUv0 = accessor(offsetof(Vertex, texCoord), kFloat, false, "VEC2");
            aUv1 = accessor(offsetof(Vertex, lightmapUV), kFloat, false, "VEC2");
            aTan = accessor(offsetof(Vertex, tangent), kFloat, false, "VEC4");
        }

        const int aIdx = int(accessors.size());
        accessors.push_back({
            {"bufferView", iViewIdx}, {"componentType", kUnsignedInt},
            {"count", indexCount}, {"type", "SCALAR"}});

        // 5. Mesh + node.
        json prim = {
            {"attributes", {
                {"POSITION", aPos}, {"NORMAL", aNrm},
                {"TEXCOORD_0", aUv0}, {"TEXCOORD_1", aUv1}, {"TANGENT", aTan}}},
            {"indices", aIdx}, {"mode", 4 /*TRIANGLES*/}};
        json jm = {{"primitives", json::array({prim})}};
        if (!m.name.empty()) jm["name"] = m.name;
        const int meshIdx = int(jmeshes.size());
        jmeshes.push_back(jm);

        json node = {{"mesh", meshIdx}};
        if (!m.name.empty()) node["name"] = m.name;
        sceneNodes.push_back(int(nodes.size()));
        nodes.push_back(node);
    }

    const size_t meshCount = jmeshes.size();
    if (meshCount == 0) {
        Log::error("MeshoptGlbExporter: nothing to export");
        return false;
    }

    json root;
    root["asset"] = {{"version", "2.0"}, {"generator", "SaidaEngine meshopt exporter"}};
    json used = json::array({"EXT_meshopt_compression"});
    if (options.quantize) used.push_back("KHR_mesh_quantization");
    root["extensionsUsed"] = used;
    root["extensionsRequired"] = used;
    root["buffers"] = json::array({
        {{"byteLength", bin.size()}},
        {{"byteLength", fallbackOffset},
         {"extensions", {{"EXT_meshopt_compression", {{"fallback", true}}}}}},
    });
    root["bufferViews"] = std::move(bufferViews);
    root["accessors"] = std::move(accessors);
    root["meshes"] = std::move(jmeshes);
    root["nodes"] = std::move(nodes);
    root["scenes"] = json::array({{{"nodes", std::move(sceneNodes)}}});
    root["scene"] = 0;

    const std::string jsonStr = root.dump();
    std::vector<uint8_t> jsonChunk(jsonStr.begin(), jsonStr.end());
    padTo4(jsonChunk, ' ');
    padTo4(bin, 0);

    const uint32_t total = 12 + 8 + uint32_t(jsonChunk.size()) + 8 + uint32_t(bin.size());

    std::ofstream os(outPath, std::ios::binary);
    if (!os) { Log::error("MeshoptGlbExporter: cannot open ", outPath); return false; }
    writeU32(os, 0x46546C67);  // "glTF" magic
    writeU32(os, 2);           // version
    writeU32(os, total);
    writeU32(os, uint32_t(jsonChunk.size()));
    writeU32(os, 0x4E4F534A);  // "JSON"
    os.write(reinterpret_cast<const char*>(jsonChunk.data()), jsonChunk.size());
    writeU32(os, uint32_t(bin.size()));
    writeU32(os, 0x004E4942);  // "BIN\0"
    os.write(reinterpret_cast<const char*>(bin.data()), bin.size());
    os.flush();
    if (!os) { Log::error("MeshoptGlbExporter: write failed for ", outPath); return false; }

    Log::info("MeshoptGlbExporter: wrote ", outPath, " (", meshCount,
              " meshes, ", options.quantize ? "quantized" : "lossless",
              ", BIN ", bin.size(), " bytes)");
    return true;
}

bool collectExportMeshes(const std::string& sourcePath, std::vector<ExportMesh>& out,
                         std::string& error) {
    const std::string ext = std::filesystem::path(sourcePath).extension().string();
    if (ext == ".obj") {
        std::ifstream in(sourcePath, std::ios::binary);
        if (!in) { error = "cannot open " + sourcePath; return false; }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
        MeshData data;
        if (!Mesh::parseObjBytes(bytes.data(), bytes.size(), data, error)) return false;
        if (data.vertices.empty() || data.indices.empty()) {
            error = "no usable geometry in " + sourcePath;
            return false;
        }
        ExportMesh mesh;
        mesh.name = std::filesystem::path(sourcePath).stem().string();
        mesh.vertices = std::move(data.vertices);
        mesh.indices = std::move(data.indices);
        out.push_back(std::move(mesh));
        return true;
    }

    cgltf_options options = {};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, sourcePath.c_str(), &data) != cgltf_result_success) {
        error = "failed to parse " + sourcePath;
        return false;
    }
    if (cgltf_load_buffers(&options, data, sourcePath.c_str()) != cgltf_result_success ||
        cgltf_validate(data) != cgltf_result_success || !decodeMeshoptBuffers(data)) {
        cgltf_free(data);
        error = "invalid or corrupt glTF: " + sourcePath;
        return false;
    }

    for (size_t m = 0; m < data->meshes_count; ++m) {
        for (size_t p = 0; p < data->meshes[m].primitives_count; ++p) {
            const cgltf_primitive& prim = data->meshes[m].primitives[p];
            if (prim.type != cgltf_primitive_type_triangles) continue;

            ExportMesh mesh;
            mesh.name = data->meshes[m].name
                ? std::string(data->meshes[m].name) + "_prim" + std::to_string(p)
                : "mesh" + std::to_string(m) + "_prim" + std::to_string(p);

            size_t vertexCount = 0;
            for (size_t a = 0; a < prim.attributes_count; ++a)
                if (prim.attributes[a].type == cgltf_attribute_type_position)
                    vertexCount = prim.attributes[a].data->count;
            if (vertexCount == 0) continue;
            mesh.vertices.resize(vertexCount);

            for (size_t a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& attr = prim.attributes[a];
                for (size_t v = 0; v < vertexCount && v < attr.data->count; ++v) {
                    Vertex& vert = mesh.vertices[v];
                    switch (attr.type) {
                    case cgltf_attribute_type_position:
                        cgltf_accessor_read_float(attr.data, v, &vert.pos.x, 3); break;
                    case cgltf_attribute_type_normal:
                        cgltf_accessor_read_float(attr.data, v, &vert.normal.x, 3); break;
                    case cgltf_attribute_type_texcoord:
                        cgltf_accessor_read_float(attr.data, v, &vert.texCoord.x, 2); break;
                    case cgltf_attribute_type_tangent:
                        cgltf_accessor_read_float(attr.data, v, &vert.tangent.x, 4); break;
                    default: break;
                    }
                }
            }

            if (prim.indices) {
                mesh.indices.resize(prim.indices->count);
                for (size_t i = 0; i < prim.indices->count; ++i)
                    mesh.indices[i] = uint32_t(cgltf_accessor_read_index(prim.indices, i));
            } else {
                mesh.indices.resize(vertexCount);
                for (size_t i = 0; i < vertexCount; ++i) mesh.indices[i] = uint32_t(i);
            }
            out.push_back(std::move(mesh));
        }
    }
    cgltf_free(data);
    if (out.empty()) {
        error = "no triangle geometry in " + sourcePath;
        return false;
    }
    return true;
}

} // namespace saida
