#include "graphics/GeometryRegistry.hpp"

#include "graphics/Buffer.hpp"
#include "graphics/Mesh.hpp" // for Vertex definition
#include "core/Log.hpp"

#ifndef SAIDA_RHI_WEBGPU
#include "graphics/VulkanDevice.hpp"
#include "vk_mem_alloc.h"
#endif

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>

namespace saida {

namespace {

// A refused upload says what was asked and what was left, so a caller can
// tell a full arena from a fragmented one.
std::string refusal(const char* what, uint64_t requested, const GeometryUsage& usage,
                    const GeometryCapacity& capacity) {
    const bool vertices = std::string(what) == "vertex";
    return std::string("failed to allocate ") + what + " space in GeometryRegistry: " +
           std::to_string(requested) + (vertices ? " vertices" : " indices") + " requested, " +
           std::to_string(vertices ? usage.vertices : usage.indices) + " of " +
           std::to_string(vertices ? capacity.vertices : capacity.indices) + " in use, largest free range " +
           std::to_string(vertices ? usage.largestFreeVertices : usage.largestFreeIndices) + ", " +
           std::to_string(usage.allocations) + " meshes resident";
}

// The arenas are copied out of as well as into: compaction moves geometry
// from the old buffers to new ones on the device.
const rhi::BufferUsage kVertexArenaUsage =
    rhi::BufferUsage::TransferSrc | rhi::BufferUsage::TransferDst | rhi::BufferUsage::Vertex;
const rhi::BufferUsage kIndexArenaUsage =
    rhi::BufferUsage::TransferSrc | rhi::BufferUsage::TransferDst | rhi::BufferUsage::Index;

} // namespace

GeometryRegistry::GeometryRegistry(rhi::Device& device, DeviceSize maxVertices, DeviceSize maxIndices)
    : device_(device), capacity_{maxVertices, maxIndices} {
    if (maxVertices == 0 || maxVertices > uint64_t(INT32_MAX) ||
        maxIndices == 0 || maxIndices > uint64_t(UINT32_MAX))
        throw std::invalid_argument("geometry capacity must fit nonzero vertex/index draw offsets");

    const uint64_t vertexBufferSize = maxVertices * sizeof(Vertex);
    const uint64_t indexBufferSize = maxIndices * sizeof(uint32_t);

    vertexBuffer_ = std::make_unique<Buffer>(device_, vertexBufferSize, kVertexArenaUsage, MemoryUsage::GpuOnly);
    indexBuffer_ = std::make_unique<Buffer>(device_, indexBufferSize, kIndexArenaUsage, MemoryUsage::GpuOnly);

#ifndef SAIDA_RHI_WEBGPU
    VmaVirtualBlockCreateInfo vbInfo{};
    vbInfo.size = vertexBufferSize;
    if (vmaCreateVirtualBlock(&vbInfo, &vertexBlock_) != VK_SUCCESS)
        throw std::runtime_error("failed to create vertex virtual block");

    VmaVirtualBlockCreateInfo ibInfo{};
    ibInfo.size = indexBufferSize;
    if (vmaCreateVirtualBlock(&ibInfo, &indexBlock_) != VK_SUCCESS)
        throw std::runtime_error("failed to create index virtual block");
#endif
}

GeometryRegistry::~GeometryRegistry() {
#ifndef SAIDA_RHI_WEBGPU
    if (vertexBlock_) {
        vmaClearVirtualBlock(vertexBlock_);
        vmaDestroyVirtualBlock(vertexBlock_);
    }
    if (indexBlock_) {
        vmaClearVirtualBlock(indexBlock_);
        vmaDestroyVirtualBlock(indexBlock_);
    }
#endif
}

#ifndef SAIDA_RHI_WEBGPU
bool GeometryRegistry::tryAllocate(GeometryAllocation& into, uint64_t vertexBytes, uint64_t indexBytes) {
    VmaVirtualAllocationCreateInfo allocInfo{};
    allocInfo.size = vertexBytes;
    allocInfo.alignment = 0;  // sizeof(Vertex) is not a power of two; offsets are divided by it
    VkDeviceSize vertexOffsetBytes = 0;
    if (vmaVirtualAllocate(vertexBlock_, &allocInfo, &into.vertexVirtualAlloc, &vertexOffsetBytes) != VK_SUCCESS)
        return false;
    allocInfo.size = indexBytes;
    allocInfo.alignment = sizeof(uint32_t);
    VkDeviceSize indexOffsetBytes = 0;
    if (vmaVirtualAllocate(indexBlock_, &allocInfo, &into.indexVirtualAlloc, &indexOffsetBytes) != VK_SUCCESS) {
        vmaVirtualFree(vertexBlock_, into.vertexVirtualAlloc);
        into.vertexVirtualAlloc = VK_NULL_HANDLE;
        return false;
    }
    into.vertexOffset = static_cast<int32_t>(vertexOffsetBytes / sizeof(Vertex));
    into.firstIndex = static_cast<uint32_t>(indexOffsetBytes / sizeof(uint32_t));
    return true;
}
#endif

void GeometryRegistry::allocate(GeometryAllocation& into, const std::vector<Vertex>& vertices,
                                const std::vector<uint32_t>& indices) {
    GeometryAllocation alloc{};
    alloc.indexCount = static_cast<uint32_t>(indices.size());
    alloc.vertexCount = static_cast<uint32_t>(vertices.size());

    const uint64_t vertexSize = vertices.size() * sizeof(Vertex);
    const uint64_t indexSize = indices.size() * sizeof(uint32_t);

#ifndef SAIDA_RHI_WEBGPU
    if (!tryAllocate(alloc, vertexSize, indexSize)) {
        const GeometryUsage before = usage();
        const bool fitsVertices = capacity_.vertices - before.vertices >= vertices.size();
        const bool fitsIndices = capacity_.indices - before.indices >= indices.size();
        // Enough room in total, only not in one piece: pack and try again.
        if (!(fitsVertices && fitsIndices && compact() && tryAllocate(alloc, vertexSize, indexSize))) {
            const GeometryUsage now = usage();
            const bool vertexShort = now.largestFreeVertices < vertices.size();
            throw std::runtime_error(vertexShort ? refusal("vertex", vertices.size(), now, capacity_)
                                                 : refusal("index", indices.size(), now, capacity_));
        }
    }
    const uint64_t vertexOffsetBytes = uint64_t(alloc.vertexOffset) * sizeof(Vertex);
    const uint64_t indexOffsetBytes = uint64_t(alloc.firstIndex) * sizeof(uint32_t);

    // Copy data to GPU
    Buffer stagingVertex(device_, vertexSize, rhi::BufferUsage::TransferSrc, MemoryUsage::HostVisible);
    stagingVertex.write(vertices.data(), vertexSize);

    Buffer stagingIndex(device_, indexSize, rhi::BufferUsage::TransferSrc, MemoryUsage::HostVisible);
    stagingIndex.write(indices.data(), indexSize);

    device_.withSingleTimeEncoder([&](rhi::CommandEncoder& enc) {
        enc.copyBufferToBuffer(stagingVertex, *vertexBuffer_, vertexSize, 0, vertexOffsetBytes);
        enc.copyBufferToBuffer(stagingIndex, *indexBuffer_, indexSize, 0, indexOffsetBytes);
    });
    into = alloc;
    live_.insert(&into);
#else
    const uint64_t vertexOffsetBytes = vertexCursorBytes_;
    const uint64_t indexOffsetBytes = indexCursorBytes_;
    vertexCursorBytes_ += vertexSize;
    indexCursorBytes_ += indexSize;
    if (vertexCursorBytes_ > vertexBuffer_->size() || indexCursorBytes_ > indexBuffer_->size())
        throw std::runtime_error("failed to allocate space in web GeometryRegistry");
    alloc.vertexVirtualAlloc = vertexOffsetBytes + 1;
    alloc.indexVirtualAlloc = indexOffsetBytes + 1;
    alloc.vertexOffset = static_cast<int32_t>(vertexOffsetBytes / sizeof(Vertex));
    alloc.firstIndex = static_cast<uint32_t>(indexOffsetBytes / sizeof(uint32_t));

    Buffer stagingVertex(device_, vertexSize, rhi::BufferUsage::TransferSrc, MemoryUsage::HostVisible);
    stagingVertex.write(vertices.data(), vertexSize);

    Buffer stagingIndex(device_, indexSize, rhi::BufferUsage::TransferSrc, MemoryUsage::HostVisible);
    stagingIndex.write(indices.data(), indexSize);

    device_.withSingleTimeEncoder([&](rhi::CommandEncoder& enc) {
        enc.copyBufferToBuffer(stagingVertex, *vertexBuffer_, vertexSize, 0, vertexOffsetBytes);
        enc.copyBufferToBuffer(stagingIndex, *indexBuffer_, indexSize, 0, indexOffsetBytes);
    });
    into = alloc;
#endif
}

bool GeometryRegistry::compact() {
#ifndef SAIDA_RHI_WEBGPU
    const auto started = std::chrono::steady_clock::now();
    const GeometryUsage before = usage();
    device_.waitIdle();

    // Oldest offsets first, so the packing is the same for the same arena.
    std::vector<GeometryAllocation*> order(live_.begin(), live_.end());
    std::sort(order.begin(), order.end(), [](const GeometryAllocation* a, const GeometryAllocation* b) {
        return a->firstIndex != b->firstIndex ? a->firstIndex < b->firstIndex : a->vertexOffset < b->vertexOffset;
    });
    struct Move { uint64_t vertexFrom, vertexTo, vertexBytes, indexFrom, indexTo, indexBytes; };
    std::vector<Move> moves;
    moves.reserve(order.size());

    vmaClearVirtualBlock(vertexBlock_);
    vmaClearVirtualBlock(indexBlock_);
    for (GeometryAllocation* a : order) {
        Move move{uint64_t(a->vertexOffset) * sizeof(Vertex), 0, uint64_t(a->vertexCount) * sizeof(Vertex),
                  uint64_t(a->firstIndex) * sizeof(uint32_t), 0, uint64_t(a->indexCount) * sizeof(uint32_t)};
        // An empty block allocated in order hands out consecutive ranges: the
        // same sizes that were resident always fit again.
        if (!tryAllocate(*a, move.vertexBytes, move.indexBytes))
            throw std::logic_error("GeometryRegistry: compaction could not place a resident mesh");
        move.vertexTo = uint64_t(a->vertexOffset) * sizeof(Vertex);
        move.indexTo = uint64_t(a->firstIndex) * sizeof(uint32_t);
        moves.push_back(move);
    }

    auto vertices = std::make_unique<Buffer>(device_, vertexBuffer_->size(), kVertexArenaUsage, MemoryUsage::GpuOnly);
    auto indices = std::make_unique<Buffer>(device_, indexBuffer_->size(), kIndexArenaUsage, MemoryUsage::GpuOnly);
    device_.withSingleTimeEncoder([&](rhi::CommandEncoder& enc) {
        for (const Move& m : moves) {
            if (m.vertexBytes) enc.copyBufferToBuffer(*vertexBuffer_, *vertices, m.vertexBytes, m.vertexFrom, m.vertexTo);
            if (m.indexBytes) enc.copyBufferToBuffer(*indexBuffer_, *indices, m.indexBytes, m.indexFrom, m.indexTo);
        }
    });
    vertexBuffer_ = std::move(vertices);
    indexBuffer_ = std::move(indices);
    ++compactions_;

    const GeometryUsage after = usage();
    Log::info("GeometryRegistry: compacted ", moves.size(), " meshes in ",
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
              " ms; largest free range ", before.largestFreeVertices, " -> ", after.largestFreeVertices,
              " vertices, ", before.largestFreeIndices, " -> ", after.largestFreeIndices, " indices");
    return true;
#else
    return false;
#endif
}

GeometryUsage GeometryRegistry::usage() const {
    GeometryUsage out;
#ifndef SAIDA_RHI_WEBGPU
    VmaDetailedStatistics vertices{}, indices{};
    vmaCalculateVirtualBlockStatistics(vertexBlock_, &vertices);
    vmaCalculateVirtualBlockStatistics(indexBlock_, &indices);
    out.vertices = vertices.statistics.allocationBytes / sizeof(Vertex);
    out.indices = indices.statistics.allocationBytes / sizeof(uint32_t);
    out.largestFreeVertices = vertices.unusedRangeCount ? vertices.unusedRangeSizeMax / sizeof(Vertex) : 0;
    out.largestFreeIndices = indices.unusedRangeCount ? indices.unusedRangeSizeMax / sizeof(uint32_t) : 0;
    out.allocations = indices.statistics.allocationCount;
#else
    out.vertices = vertexCursorBytes_ / sizeof(Vertex);
    out.indices = indexCursorBytes_ / sizeof(uint32_t);
    out.largestFreeVertices = capacity_.vertices - out.vertices;
    out.largestFreeIndices = capacity_.indices - out.indices;
#endif
    return out;
}

void GeometryRegistry::free(GeometryAllocation& alloc) {
#ifndef SAIDA_RHI_WEBGPU
    if (alloc.vertexVirtualAlloc) vmaVirtualFree(vertexBlock_, alloc.vertexVirtualAlloc);
    if (alloc.indexVirtualAlloc) vmaVirtualFree(indexBlock_, alloc.indexVirtualAlloc);
    live_.erase(&alloc);
#endif
    // Linear allocator on the web: memory is reclaimed with the registry.
    alloc = GeometryAllocation{};
}

} // namespace saida
