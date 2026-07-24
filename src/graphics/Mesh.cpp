#include "graphics/Mesh.hpp"

#include "core/Profiler.hpp"
#include "graphics/Buffer.hpp"
#include "tiny_obj_loader.h"

#ifndef SAIDA_RHI_WEBGPU
#include "graphics/VulkanDevice.hpp"
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <limits>
#include <streambuf>
#include <unordered_map>

namespace saida {

#ifndef SAIDA_RHI_WEBGPU
VkVertexInputBindingDescription Vertex::bindingDescription() {
    VkVertexInputBindingDescription desc{};
    desc.binding = 0;
    desc.stride = sizeof(Vertex);
    desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return desc;
}

std::array<VkVertexInputAttributeDescription, 8> Vertex::attributeDescriptions() {
    std::array<VkVertexInputAttributeDescription, 8> attrs{};
    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = offsetof(Vertex, pos);
    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset = offsetof(Vertex, normal);
    attrs[2].binding = 0;
    attrs[2].location = 2;
    attrs[2].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[2].offset = offsetof(Vertex, color);
    attrs[3].binding = 0;
    attrs[3].location = 3;
    attrs[3].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[3].offset = offsetof(Vertex, texCoord);
    attrs[4].binding = 0;
    attrs[4].location = 4;
    attrs[4].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[4].offset = offsetof(Vertex, lightmapUV);
    attrs[5].binding = 0;
    attrs[5].location = 5;
    attrs[5].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[5].offset = offsetof(Vertex, tangent);
    attrs[6].binding = 0;
    attrs[6].location = 6;
    attrs[6].format = VK_FORMAT_R32G32B32A32_SINT;
    attrs[6].offset = offsetof(Vertex, boneIndices);
    attrs[7].binding = 0;
    attrs[7].location = 7;
    attrs[7].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[7].offset = offsetof(Vertex, boneWeights);
    return attrs;
}
#endif

Mesh::Mesh(GeometryRegistry& registry, const std::vector<Vertex>& vertices,
           const std::vector<uint32_t>& indices)
    : registry_(registry) {
    upload(vertices, indices);
}

Mesh::Mesh(GeometryRegistry& registry) : registry_(registry) {}

void Mesh::upload(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    // Cache the local-space AABB while the vertices are still on the CPU (they
    // are GPU-only after allocate). Used by the physics layer for auto-shape.
    if (!vertices.empty()) {
        glm::vec3 mn(std::numeric_limits<float>::max());
        glm::vec3 mx(std::numeric_limits<float>::lowest());
        for (const Vertex& v : vertices) {
            mn = glm::min(mn, v.pos);
            mx = glm::max(mx, v.pos);
        }
        bounds_.min = mn;
        bounds_.max = mx;
    }

    // Retain positions + indices on the CPU for collider generation (the GPU
    // buffers are write-only from here on).
    collisionVertices_.clear();
    collisionVertices_.reserve(vertices.size());
    for (const Vertex& v : vertices) collisionVertices_.push_back(v.pos);
    collisionIndices_ = indices;

    if (allocation_.indexCount != 0) registry_.free(allocation_);
    allocation_ = registry_.allocate(vertices, indices);
    gpuBytes_ = static_cast<uint64_t>(vertices.size()) * sizeof(Vertex) +
                static_cast<uint64_t>(indices.size()) * sizeof(uint32_t);
}

Mesh::~Mesh() {
    registry_.free(allocation_);
}

namespace {
// Zero-copy streambuf view over a memory block (parses .obj from
// AssetLoader bytes).
struct MemoryStreamBuf : std::streambuf {
    MemoryStreamBuf(const uint8_t* data, size_t size) {
        char* p = const_cast<char*>(reinterpret_cast<const char*>(data));
        setg(p, p, p + size);
    }
};
} // namespace

bool Mesh::parseObjBytes(const uint8_t* data, size_t size, MeshData& out, std::string& error) {
    SAIDA_PROFILE_SCOPE("Resource/ParseObjMesh");

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    MemoryStreamBuf buf(data, size);
    std::istream stream(&buf);
    // No MaterialReader: .mtl files are not resolved (the engine's materials
    // come from scenes, not .obj files). triangulate=true by default.
    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, &stream)) {
        error = warn + err;
        return false;
    }

    // Compute the bounding box to recenter the model on the origin and scale it
    // uniformly so its largest dimension fits within a unit cube.
    glm::vec3 mn(std::numeric_limits<float>::max());
    glm::vec3 mx(std::numeric_limits<float>::lowest());
    for (size_t i = 0; i + 2 < attrib.vertices.size(); i += 3) {
        glm::vec3 p(attrib.vertices[i], attrib.vertices[i + 1], attrib.vertices[i + 2]);
        mn = glm::min(mn, p);
        mx = glm::max(mx, p);
    }
    glm::vec3 center = (mn + mx) * 0.5f;
    glm::vec3 dim = mx - mn;
    float extent = std::max({dim.x, dim.y, dim.z});
    float scale = extent > 0.0f ? 1.0f / extent : 1.0f;

    std::vector<Vertex>& vertices = out.vertices;
    std::vector<uint32_t>& indices = out.indices;
    vertices.clear();
    indices.clear();
    std::unordered_map<uint64_t, uint32_t> uniqueVertices;

    for (const auto& shape : shapes) {
        for (const auto& idx : shape.mesh.indices) {
            // De-duplicate on the (position, normal) index pair.
            uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(idx.vertex_index)) << 32)
                         | static_cast<uint32_t>(idx.normal_index);
            if (auto it = uniqueVertices.find(key); it != uniqueVertices.end()) {
                indices.push_back(it->second);
                continue;
            }

            Vertex v{};
            v.pos = (glm::vec3(attrib.vertices[3 * idx.vertex_index + 0],
                               attrib.vertices[3 * idx.vertex_index + 1],
                               attrib.vertices[3 * idx.vertex_index + 2]) - center) * scale;
            if (idx.normal_index >= 0) {
                v.normal = glm::vec3(attrib.normals[3 * idx.normal_index + 0],
                                     attrib.normals[3 * idx.normal_index + 1],
                                     attrib.normals[3 * idx.normal_index + 2]);
            } else {
                v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
            }
            v.color = glm::vec3(1.0f);  // white albedo; lighting provides shading
            if (idx.texcoord_index >= 0) {
                v.texCoord = glm::vec2(attrib.texcoords[2 * idx.texcoord_index + 0],
                                       1.0f - attrib.texcoords[2 * idx.texcoord_index + 1]);
            } else {
                v.texCoord = glm::vec2(0.0f);
            }
            // Keep a secondary UV channel available for future tooling; OBJ
            // meshes mirror their texture UVs by default.
            v.lightmapUV = v.texCoord;

            uint32_t newIndex = static_cast<uint32_t>(vertices.size());
            uniqueVertices[key] = newIndex;
            vertices.push_back(v);
            indices.push_back(newIndex);
        }
    }

    // Compute tangents
    for (size_t i = 0; i < indices.size(); i += 3) {
        Vertex& v0 = vertices[indices[i]];
        Vertex& v1 = vertices[indices[i+1]];
        Vertex& v2 = vertices[indices[i+2]];

        glm::vec3 e1 = v1.pos - v0.pos;
        glm::vec3 e2 = v2.pos - v0.pos;
        glm::vec2 dUV1 = v1.texCoord - v0.texCoord;
        glm::vec2 dUV2 = v2.texCoord - v0.texCoord;

        float f = 1.0f / (dUV1.x * dUV2.y - dUV2.x * dUV1.y);
        glm::vec3 tangent(0.0f);
        if (!std::isinf(f) && !std::isnan(f)) {
            tangent = (e1 * dUV2.y - e2 * dUV1.y) * f;
        }

        v0.tangent += glm::vec4(tangent, 0.0f);
        v1.tangent += glm::vec4(tangent, 0.0f);
        v2.tangent += glm::vec4(tangent, 0.0f);
    }

    for (auto& v : vertices) {
        glm::vec3 n = v.normal;
        glm::vec3 t = glm::vec3(v.tangent);
        if (glm::length(t) < 0.0001f) {
            glm::vec3 c1 = glm::cross(n, glm::vec3(0.0f, 0.0f, 1.0f));
            glm::vec3 c2 = glm::cross(n, glm::vec3(0.0f, 1.0f, 0.0f));
            t = glm::length(c1) > glm::length(c2) ? c1 : c2;
        }
        v.tangent = glm::vec4(glm::normalize(t - n * glm::dot(n, t)), 1.0f);
    }

    return true;
}

void Mesh::bind(rhi::RenderPassEncoder& rp) const {
    // In hybrid rendering, binding is usually done globally, but if we do it per-mesh:
    rp.setVertexBuffer(*registry_.vertexBuffer());
    rp.setIndexBuffer(*registry_.indexBuffer());
}

void Mesh::draw(rhi::RenderPassEncoder& rp) const {
    rp.drawIndexed(allocation_.indexCount, 1, allocation_.firstIndex, allocation_.vertexOffset, 0);
}

} // namespace saida
