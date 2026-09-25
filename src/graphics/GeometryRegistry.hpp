#pragma once

#ifndef SAIDA_RHI_WEBGPU
#include "graphics/VmaFwd.hpp"
#include <vulkan/vulkan.h>
#else
#include "graphics/Buffer.hpp"
#endif

#include <memory>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "rhi/Rhi.hpp"
#include "graphics/GeometryCapacity.hpp"

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
    uint32_t vertexCount = 0;
};

// Manages a single unified vertex buffer and index buffer, sub-allocating space
// for individual meshes using VMA's Virtual Blocks.
//
// The registry knows every allocation it made and where its owner keeps it,
// so it can move them: when an upload does not fit in any free range but the
// arena's total free space would hold it, the live geometry is packed
// together (GeometryRegistry::compact) and the upload retried. A streaming
// world frees and allocates meshes of every size for as long as it runs, and
// without this its free space ends in pieces none of which fits the next tile.
class GeometryRegistry {
public:
    using DeviceSize = uint64_t;
    static constexpr DeviceSize kDefaultMaxVertices = GeometryCapacity{}.vertices;
    static constexpr DeviceSize kDefaultMaxIndices = GeometryCapacity{}.indices;

    // Default to ~64MB vertices (1M vertices) and ~12MB indices (3M indices).
    GeometryRegistry(rhi::Device& device,
                     DeviceSize maxVertices = kDefaultMaxVertices,
                     DeviceSize maxIndices = kDefaultMaxIndices);
    ~GeometryRegistry();

    GeometryRegistry(const GeometryRegistry&) = delete;
    GeometryRegistry& operator=(const GeometryRegistry&) = delete;

    // Allocates space in `into`, uploads the geometry, and keeps `into` up to
    // date if the arena is later compacted: `into` must stay at the same
    // address until it is freed (a Mesh member, which is never copied).
    void allocate(GeometryAllocation& into, const std::vector<Vertex>& vertices,
                  const std::vector<uint32_t>& indices);

    // Frees a previously allocated geometry region and forgets `alloc`.
    void free(GeometryAllocation& alloc);

    // Packs every live allocation at the start of the arenas, largest free
    // range last. Waits for the device to be idle first: it moves geometry the
    // frames in flight may still be reading. Returns false where the backend
    // cannot move geometry (WebGPU's linear arena).
    bool compact();

    Buffer* vertexBuffer() const { return vertexBuffer_.get(); }
    Buffer* indexBuffer() const { return indexBuffer_.get(); }
    GeometryCapacity capacity() const { return capacity_; }
    GeometryUsage usage() const;
    uint32_t compactions() const { return compactions_; }

private:
#ifndef SAIDA_RHI_WEBGPU
    bool tryAllocate(GeometryAllocation& into, uint64_t vertexBytes, uint64_t indexBytes);
#endif

    rhi::Device& device_;
    GeometryCapacity capacity_;
    std::unique_ptr<Buffer> vertexBuffer_;
    std::unique_ptr<Buffer> indexBuffer_;
    uint32_t compactions_ = 0;
#ifndef SAIDA_RHI_WEBGPU
    VmaVirtualBlock vertexBlock_ = VK_NULL_HANDLE;
    VmaVirtualBlock indexBlock_ = VK_NULL_HANDLE;
    std::unordered_set<GeometryAllocation*> live_;
#else
    uint64_t vertexCursorBytes_ = 0;
    uint64_t indexCursorBytes_ = 0;
#endif
};

} // namespace saida
