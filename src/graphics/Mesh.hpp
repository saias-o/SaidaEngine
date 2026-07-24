#pragma once

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "graphics/GeometryRegistry.hpp"
#include "rhi/Rhi.hpp"

#ifndef SAIDA_RHI_WEBGPU
#include <vulkan/vulkan.h>
#endif

namespace saida {

class VulkanDevice;
class GeometryRegistry;

struct Vertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec3 color;
    glm::vec2 texCoord;
    glm::vec2 lightmapUV;  // secondary UV set, mirrored from texture UV by default
    glm::vec4 tangent;
    glm::ivec4 boneIndices{-1, -1, -1, -1};
    glm::vec4 boneWeights{0.0f, 0.0f, 0.0f, 0.0f};

#ifndef SAIDA_RHI_WEBGPU
    static VkVertexInputBindingDescription bindingDescription();
    static std::array<VkVertexInputAttributeDescription, 8> attributeDescriptions();
#endif
};

// Local-space axis-aligned bounding box, in the mesh's own coordinate space
// (before any node transform). Computed once at construction; the physics layer
// uses it to auto-detect collider shapes without keeping a CPU vertex copy.
struct Aabb {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};

    glm::vec3 center() const { return (min + max) * 0.5f; }
    glm::vec3 extent() const { return max - min; }  // full size along each axis
};

// CPU geometry produced by a parse (.obj on the AssetLoader worker) —
// ready to be uploaded into a Mesh on the main thread.
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

// Owns device-local vertex and index buffers and knows how to bind/draw itself.
class Mesh {
public:
    Mesh(GeometryRegistry& registry, const std::vector<Vertex>& vertices,
         const std::vector<uint32_t>& indices);
    // Empty proxy (async-loading placeholder): a stable pointer returned
    // immediately while a load happens asynchronously. draw() is a no-op
    // until upload() has filled in the geometry; bounds() and
    // collisionVertices() are empty.
    explicit Mesh(GeometryRegistry& registry);
    ~Mesh();
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    // Fills in a proxy (allocates + uploads the geometry, computes bounds
    // and collision data). Called on the main thread once the asynchronous
    // parse has finished.
    void upload(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);

    // False until a proxy has received its geometry.
    bool loaded() const { return allocation_.indexCount != 0; }

    // GPU bytes used by the geometry (asset accounting for GPU resource tracking).
    uint64_t gpuBytes() const { return gpuBytes_; }

    // Parses a .obj from memory — pure CPU, safe off the main thread.
    // Does not resolve .mtl files (the engine's materials come from scenes).
    static bool parseObjBytes(const uint8_t* data, size_t size, MeshData& out, std::string& error);

    void bind(rhi::RenderPassEncoder& rp) const;
    void draw(rhi::RenderPassEncoder& rp) const;

    GeometryAllocation geometryAllocation() const { return allocation_; }
    const GeometryAllocation& allocation() const { return allocation_; }

    const Aabb& bounds() const { return bounds_; }

    // Lightweight CPU copy of the geometry (positions + indices only) kept for the
    // physics layer to build convex-hull / triangle-mesh colliders. (~12 B/vertex
    // + 4 B/index; could be made opt-in for shipping/mobile.)
    const std::vector<glm::vec3>& collisionVertices() const { return collisionVertices_; }
    const std::vector<uint32_t>& collisionIndices() const { return collisionIndices_; }

private:
    GeometryRegistry& registry_;
    GeometryAllocation allocation_;
    Aabb bounds_;
    uint64_t gpuBytes_ = 0;
    std::vector<glm::vec3> collisionVertices_;
    std::vector<uint32_t> collisionIndices_;
};

} // namespace saida
