#include "rhi/vulkan/BindGroup.hpp"

#include "graphics/Buffer.hpp"
#include "graphics/VulkanDevice.hpp"
#include "rhi/vulkan/Convert.hpp"

#include <stdexcept>

namespace saida::rhi::vulkan {

namespace {
// Sets per internal pool chunk. Small groups (per-frame globals) fit the first
// chunk; heavy users (one set per material) just grow more chunks.
constexpr uint32_t kSetsPerPool = 16;
}

BindGroupLayout::BindGroupLayout(VulkanDevice& device, std::vector<rhi::BindGroupLayoutEntry> entries)
    : device_(device), entries_(std::move(entries)) {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(entries_.size());
    for (const auto& entry : entries_) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = entry.binding;
        b.descriptorType = toVk(entry.type);
        b.descriptorCount = entry.count;
        b.stageFlags = toVk(entry.visibility);
        bindings.push_back(b);
    }

    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = static_cast<uint32_t>(bindings.size());
    ci.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device_.device(), &ci, nullptr, &layout_) != VK_SUCCESS)
        throw std::runtime_error("failed to create bind group layout");
}

BindGroupLayout::~BindGroupLayout() {
    for (const Pool& pool : pools_)
        vkDestroyDescriptorPool(device_.device(), pool.handle, nullptr);
    vkDestroyDescriptorSetLayout(device_.device(), layout_, nullptr);
}

void BindGroupLayout::addPool() {
    std::vector<VkDescriptorPoolSize> sizes;
    sizes.reserve(entries_.size());
    for (const auto& entry : entries_)
        sizes.push_back({toVk(entry.type), entry.count * kSetsPerPool});

    VkDescriptorPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    // BindGroups free their set on destruction (material unload, target resize).
    ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    ci.poolSizeCount = static_cast<uint32_t>(sizes.size());
    ci.pPoolSizes = sizes.data();
    ci.maxSets = kSetsPerPool;

    Pool pool{};
    pool.remaining = kSetsPerPool;
    if (vkCreateDescriptorPool(device_.device(), &ci, nullptr, &pool.handle) != VK_SUCCESS)
        throw std::runtime_error("failed to create bind group pool");
    pools_.push_back(pool);
}

VkDescriptorSet BindGroupLayout::allocate(VkDescriptorPool& outPool) {
    if (pools_.empty() || pools_.back().remaining == 0) addPool();
    Pool& pool = pools_.back();

    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool.handle;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout_;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_.device(), &ai, &set) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate bind group");
    pool.remaining--;
    outPool = pool.handle;
    return set;
}

void BindGroupLayout::free(VkDescriptorPool pool, VkDescriptorSet set) {
    vkFreeDescriptorSets(device_.device(), pool, 1, &set);
    for (Pool& p : pools_) {
        if (p.handle == pool) {
            p.remaining++;
            break;
        }
    }
}

BindGroup::BindGroup(BindGroupLayout& layout, const std::vector<BindGroupEntry>& entries)
    : layout_(layout) {
    set_ = layout_.allocate(pool_);

    // Info arrays must outlive vkUpdateDescriptorSets; reserve so pointers into
    // them stay stable while writes accumulate.
    std::vector<VkDescriptorBufferInfo> bufferInfos;
    std::vector<VkDescriptorImageInfo> imageInfos;
    bufferInfos.reserve(entries.size());
    imageInfos.reserve(entries.size());
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(entries.size());

    for (const BindGroupEntry& entry : entries) {
        const rhi::BindGroupLayoutEntry* layoutEntry = nullptr;
        for (const auto& le : layout_.entries())
            if (le.binding == entry.binding) { layoutEntry = &le; break; }
        if (!layoutEntry)
            throw std::runtime_error("bind group entry has no matching layout binding");

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set_;
        write.dstBinding = entry.binding;
        write.descriptorType = toVk(layoutEntry->type);
        write.descriptorCount = 1;

        switch (layoutEntry->type) {
            case rhi::BindingType::UniformBuffer:
            case rhi::BindingType::StorageBuffer: {
                VkDescriptorBufferInfo info{};
                info.buffer = entry.buffer->handle();
                info.offset = entry.offset;
                info.range = entry.range == 0 ? VK_WHOLE_SIZE : entry.range;
                bufferInfos.push_back(info);
                write.pBufferInfo = &bufferInfos.back();
                break;
            }
            case rhi::BindingType::CombinedImageSampler:
            case rhi::BindingType::SampledTexture:
            case rhi::BindingType::Sampler:
            case rhi::BindingType::StorageImage: {
                VkDescriptorImageInfo info{};
                if (layoutEntry->type == rhi::BindingType::Sampler)
                    info.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // no image bound
                else if (layoutEntry->type == rhi::BindingType::StorageImage)
                    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;    // storage is always GENERAL
                else
                    info.imageLayout = stateInfo(entry.textureState).layout;
                info.imageView = entry.view;
                info.sampler = entry.sampler;
                imageInfos.push_back(info);
                write.pImageInfo = &imageInfos.back();
                break;
            }
        }
        writes.push_back(write);
    }

    vkUpdateDescriptorSets(layout_.device_.device(), static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

BindGroup::~BindGroup() {
    if (set_) layout_.free(pool_, set_);
}

} // namespace saida::rhi::vulkan
