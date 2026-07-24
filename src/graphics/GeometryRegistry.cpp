#include "graphics/GeometryRegistry.hpp"

#include "graphics/Buffer.hpp"
#include "graphics/Mesh.hpp" // for Vertex definition

#ifndef SAIDA_RHI_WEBGPU
#include "graphics/VulkanDevice.hpp"
#include "vk_mem_alloc.h"
#endif

#include <stdexcept>

namespace saida {

GeometryRegistry::GeometryRegistry(rhi::Device& device, DeviceSize maxVertices, DeviceSize maxIndices)
    : device_(device) {
    
    const uint64_t vertexBufferSize = maxVertices * sizeof(Vertex);
    const uint64_t indexBufferSize = maxIndices * sizeof(uint32_t);

    // We use transfer_dst for copying data in, and vertex/index for drawing.
    // If BDA is enabled, we could also add SHADER_DEVICE_ADDRESS bit.
    vertexBuffer_ = std::make_unique<Buffer>(device_, vertexBufferSize,
        rhi::BufferUsage::TransferDst | rhi::BufferUsage::Vertex,
        MemoryUsage::GpuOnly);

    indexBuffer_ = std::make_unique<Buffer>(device_, indexBufferSize,
        rhi::BufferUsage::TransferDst | rhi::BufferUsage::Index,
        MemoryUsage::GpuOnly);

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
    if (vertexBlock_) vmaDestroyVirtualBlock(vertexBlock_);
    if (indexBlock_) vmaDestroyVirtualBlock(indexBlock_);
#endif
}

GeometryAllocation GeometryRegistry::allocate(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    GeometryAllocation alloc{};
    alloc.indexCount = static_cast<uint32_t>(indices.size());

    const uint64_t vertexSize = vertices.size() * sizeof(Vertex);
    const uint64_t indexSize = indices.size() * sizeof(uint32_t);

#ifndef SAIDA_RHI_WEBGPU
    VmaVirtualAllocationCreateInfo allocInfo{};
    
    // Allocate vertex block
    allocInfo.size = vertexSize;
    allocInfo.alignment = 0; // Must be power of 2 or 0. sizeof(Vertex) is 68 (not a power of 2)!
    VkDeviceSize vertexOffsetBytes;
    if (vmaVirtualAllocate(vertexBlock_, &allocInfo, &alloc.vertexVirtualAlloc, &vertexOffsetBytes) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate vertex space in GeometryRegistry");
    alloc.vertexOffset = static_cast<int32_t>(vertexOffsetBytes / sizeof(Vertex));

    // Allocate index block
    allocInfo.size = indexSize;
    allocInfo.alignment = sizeof(uint32_t);
    VkDeviceSize indexOffsetBytes;
    if (vmaVirtualAllocate(indexBlock_, &allocInfo, &alloc.indexVirtualAlloc, &indexOffsetBytes) != VK_SUCCESS) {
        vmaVirtualFree(vertexBlock_, alloc.vertexVirtualAlloc);
        throw std::runtime_error("failed to allocate index space in GeometryRegistry");
    }
    alloc.firstIndex = static_cast<uint32_t>(indexOffsetBytes / sizeof(uint32_t));

    // Copy data to GPU
    Buffer stagingVertex(device_, vertexSize, rhi::BufferUsage::TransferSrc, MemoryUsage::HostVisible);
    stagingVertex.write(vertices.data(), vertexSize);

    Buffer stagingIndex(device_, indexSize, rhi::BufferUsage::TransferSrc, MemoryUsage::HostVisible);
    stagingIndex.write(indices.data(), indexSize);

    device_.withSingleTimeEncoder([&](rhi::CommandEncoder& enc) {
        enc.copyBufferToBuffer(stagingVertex, *vertexBuffer_, vertexSize, 0, vertexOffsetBytes);
        enc.copyBufferToBuffer(stagingIndex, *indexBuffer_, indexSize, 0, indexOffsetBytes);
    });
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
#endif

    return alloc;
}

void GeometryRegistry::free(const GeometryAllocation& alloc) {
#ifndef SAIDA_RHI_WEBGPU
    if (alloc.vertexVirtualAlloc) {
        vmaVirtualFree(vertexBlock_, alloc.vertexVirtualAlloc);
    }
    if (alloc.indexVirtualAlloc) {
        vmaVirtualFree(indexBlock_, alloc.indexVirtualAlloc);
    }
#else
    (void)alloc; // Linear allocator for web v1; memory is reclaimed with the registry.
#endif
}

} // namespace saida
