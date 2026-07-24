#pragma once

#include "rhi/BindGroup.hpp"
#include "rhi/CommandTypes.hpp"

#include <vulkan/vulkan.h>

#include <vector>

// Descriptor pools stay backend-internal; immutable groups keep the RHI portable.

namespace saida {
class Buffer;
class VulkanDevice;
}

namespace saida::rhi::vulkan {

class BindGroup;

class BindGroupLayout {
public:
    BindGroupLayout(VulkanDevice& device, std::vector<rhi::BindGroupLayoutEntry> entries);
    ~BindGroupLayout();
    BindGroupLayout(const BindGroupLayout&) = delete;
    BindGroupLayout& operator=(const BindGroupLayout&) = delete;

    VkDescriptorSetLayout handle() const { return layout_; }

    const std::vector<rhi::BindGroupLayoutEntry>& entries() const { return entries_; }

private:
    friend class BindGroup;
    VkDescriptorSet allocate(VkDescriptorPool& outPool);
    void free(VkDescriptorPool pool, VkDescriptorSet set);
    void addPool();

    VulkanDevice& device_;
    std::vector<rhi::BindGroupLayoutEntry> entries_;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    struct Pool {
        VkDescriptorPool handle = VK_NULL_HANDLE;
        uint32_t remaining = 0;
    };
    std::vector<Pool> pools_;
};

struct BindGroupEntry {
    uint32_t binding = 0;

    const saida::Buffer* buffer = nullptr;
    uint64_t offset = 0;
    uint64_t range = 0;

    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    rhi::ResourceState textureState = rhi::ResourceState::ShaderRead;
};

// The raw form supports the bindless descriptor set outside the portable RHI.
struct BindGroupLayoutRef {
    const BindGroupLayout* layout = nullptr;
    VkDescriptorSetLayout raw = VK_NULL_HANDLE;

    BindGroupLayoutRef(const BindGroupLayout* l) : layout(l) {}
    BindGroupLayoutRef(const BindGroupLayout& l) : layout(&l) {}
    BindGroupLayoutRef(VkDescriptorSetLayout r) : raw(r) {}

    VkDescriptorSetLayout handle() const { return layout ? layout->handle() : raw; }
};

class BindGroup {
public:
    BindGroup(BindGroupLayout& layout, const std::vector<BindGroupEntry>& entries);
    ~BindGroup();
    BindGroup(const BindGroup&) = delete;
    BindGroup& operator=(const BindGroup&) = delete;

    VkDescriptorSet handle() const { return set_; }

private:
    BindGroupLayout& layout_;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
};

} // namespace saida::rhi::vulkan
