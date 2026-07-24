#pragma once

#ifndef SAIDA_RHI_WEBGPU
#include "graphics/VmaFwd.hpp"
#include <vulkan/vulkan.h>
#else
#include "graphics/Buffer.hpp"
#endif

#include <memory>
#include <cstdint>
#include <vector>

#include "rhi/Rhi.hpp"

namespace saida {

struct Vertex;

struct GeometryAllocation {
#ifndef SAIDA_RHI_WEBGPU
    VmaVirtualAllocation vertexVirtualAlloc = VK_NULL_HANDLE;
    VmaVirtualAllocation indexVirtualAlloc = VK_NULL_HANDLE;
#else
    uint64_t vertexVirtualAlloc = 0;
    uint64_t indexVirtualAlloc = 0;
#endif
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int32_t vertexOffset = 0;
};

// Manages a single unified vertex buffer and index buffer, sub-allocating space
// for individual meshes using VMA's Virtual Blocks.
class GeometryRegistry {
public:
    using DeviceSize = uint64_t;
    static constexpr DeviceSize kDefaultMaxVertices = 1 * 1024 * 1024;
    static constexpr DeviceSize kDefaultMaxIndices = 3 * 1024 * 1024;

    // Default to ~64MB vertices (1M vertices) and ~12MB indices (3M indices).
    GeometryRegistry(rhi::Device& device,
                     DeviceSize maxVertices = kDefaultMaxVertices,
                     DeviceSize maxIndices = kDefaultMaxIndices);
    ~GeometryRegistry();

    GeometryRegistry(const GeometryRegistry&) = delete;
    GeometryRegistry& operator=(const GeometryRegistry&) = delete;

    // Allocates space and uploads the geometry to the unified buffers.
    GeometryAllocation allocate(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);

    // Frees a previously allocated geometry region.
    void free(const GeometryAllocation& alloc);

    Buffer* vertexBuffer() const { return vertexBuffer_.get(); }
    Buffer* indexBuffer() const { return indexBuffer_.get(); }

private:
    rhi::Device& device_;
    std::unique_ptr<Buffer> vertexBuffer_;
    std::unique_ptr<Buffer> indexBuffer_;
#ifndef SAIDA_RHI_WEBGPU
    VmaVirtualBlock vertexBlock_ = VK_NULL_HANDLE;
    VmaVirtualBlock indexBlock_ = VK_NULL_HANDLE;
#else
    uint64_t vertexCursorBytes_ = 0;
    uint64_t indexCursorBytes_ = 0;
#endif
};

} // namespace saida
