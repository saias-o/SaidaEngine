#include "graphics/BindlessTables.hpp"

#include "core/Log.hpp"
#include "graphics/Buffer.hpp"
#include "graphics/Texture.hpp"

#ifndef SAIDA_RHI_WEBGPU
#include "graphics/VulkanDevice.hpp"
#endif

#include <array>
#include <cstring>
#include <stdexcept>

namespace saida {

namespace {
// Mirror of the std430 MaterialData block consumed by shader.frag.
struct MaterialData {
    glm::vec4 baseColor;
    float metallic;
    float roughness;
    float ao;
    uint32_t albedoTexIdx;
    uint32_t normalTexIdx;
    uint32_t mrTexIdx;
    uint32_t emissiveTexIdx;
    uint32_t materialType;
    glm::vec4 emissive;
};
static_assert(sizeof(MaterialData) == 64, "MaterialData must match shader.frag std430 layout");
} // namespace

BindlessTables::BindlessTables(rhi::Device& device, uint32_t maxTextures,
                               uint32_t maxMaterials)
    : device_(device), maxTextures_(maxTextures), maxMaterials_(maxMaterials) {}

BindlessTables::~BindlessTables() {
#ifndef SAIDA_RHI_WEBGPU
    if (layout_) vkDestroyDescriptorSetLayout(device_.device(), layout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(device_.device(), pool_, nullptr);
#endif
    // materialBuffer_ destructs after this body — the same order the previous
    // owner used (layout/pool first, then the SSBO).
}

bool BindlessTables::active() const {
#ifdef SAIDA_RHI_WEBGPU
    return false;
#else
    return set_ != VK_NULL_HANDLE;
#endif
}

void BindlessTables::create() {
#ifdef SAIDA_RHI_WEBGPU
    return;
#else
    if (!device_.capabilities().descriptorIndexing) {
        Log::warn("Descriptor Indexing not supported. GPU-driven rendering is disabled.");
        return;
    }

    // Layout: Binding 0 = array of textures, Binding 1 = MaterialData SSBO
    VkDescriptorSetLayoutBinding texturesBinding{};
    texturesBinding.binding = 0;
    texturesBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    texturesBinding.descriptorCount = maxTextures_;
    texturesBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding materialBufferBinding{};
    materialBufferBinding.binding = 1;
    materialBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    materialBufferBinding.descriptorCount = 1;
    materialBufferBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    std::array<VkDescriptorSetLayoutBinding, 2> bindings = {texturesBinding, materialBufferBinding};

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    std::array<VkDescriptorBindingFlags, 2> bindingFlags = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        0
    };
    flagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
    flagsInfo.pBindingFlags = bindingFlags.data();

    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    ci.bindingCount = static_cast<uint32_t>(bindings.size());
    ci.pBindings = bindings.data();
    ci.pNext = &flagsInfo;

    if (vkCreateDescriptorSetLayout(device_.device(), &ci, nullptr, &layout_) != VK_SUCCESS)
        throw std::runtime_error("failed to create global bindless descriptor set layout");

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = maxTextures_;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();

    if (vkCreateDescriptorPool(device_.device(), &poolInfo, nullptr, &pool_) != VK_SUCCESS)
        throw std::runtime_error("failed to create global bindless pool");

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout_;

    if (vkAllocateDescriptorSets(device_.device(), &allocInfo, &set_) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate global bindless descriptor set");

    // Create global MaterialData SSBO
    VkDeviceSize ssboSize = maxMaterials_ * sizeof(MaterialData);
    materialBuffer_ = std::make_unique<Buffer>(device_, ssboSize,
        rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst,
        MemoryUsage::HostVisible); // MemoryUsage::HostVisible for simple CPU-side mapping

    // Write the SSBO to the descriptor set
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = materialBuffer_->handle();
    bufferInfo.offset = 0;
    bufferInfo.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set_;
    write.dstBinding = 1;
    write.dstArrayElement = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &bufferInfo;

    vkUpdateDescriptorSets(device_.device(), 1, &write, 0, nullptr);
#endif
}

uint32_t BindlessTables::allocTextureIndex(Texture* texture) {
#ifdef SAIDA_RHI_WEBGPU
    (void)texture;
    return 0;
#else
    if (!set_) return 0;
    if (!texture) return 0;

    // Reuses an index recycled by the graveyard before growing the table.
    uint32_t index;
    if (!freeTextureIndices_.empty()) {
        index = freeTextureIndices_.back();
        freeTextureIndices_.pop_back();
    } else {
        if (nextTextureIndex_ >= maxTextures_)
            throw std::runtime_error("bindless texture table exhausted");
        index = nextTextureIndex_++;
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = texture->imageView();
    imageInfo.sampler = texture->sampler();

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set_;
    write.dstBinding = 0;
    write.dstArrayElement = index;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(device_.device(), 1, &write, 0, nullptr);

    return index;
#endif
}

uint32_t BindlessTables::ensureTextureIndex(Texture* texture) {
#ifdef SAIDA_RHI_WEBGPU
    (void)texture;
    return 0;
#else
    if (!texture) return 0;
    if (!set_) return 0;
    if (texture->bindlessIndex() == ~0u) {
        texture->setBindlessIndex(allocTextureIndex(texture));
    }
    return texture->bindlessIndex();
#endif
}

void BindlessTables::recycleTextureIndex(uint32_t index, Texture* defaultWhite) {
#ifndef SAIDA_RHI_WEBGPU
    // The index is repointed at the white texture before it becomes
    // allocatable again: an in-flight frame may still sample this slot.
    if (set_ && defaultWhite) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = defaultWhite->imageView();
        imageInfo.sampler = defaultWhite->sampler();
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set_;
        write.dstBinding = 0;
        write.dstArrayElement = index;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device_.device(), 1, &write, 0, nullptr);
    }
#else
    (void)defaultWhite;
#endif
    freeTextureIndices_.push_back(index);
}

uint32_t BindlessTables::allocMaterialSlot(const glm::vec4& baseColor, const glm::vec4& emissive,
                                           float metallic, float roughness, float ao,
                                           uint32_t albedoIdx, uint32_t normalIdx, uint32_t mrIdx,
                                           uint32_t emissiveIdx, MaterialType type) {
#ifdef SAIDA_RHI_WEBGPU
    (void)baseColor;
    (void)emissive;
    (void)metallic;
    (void)roughness;
    (void)ao;
    (void)albedoIdx;
    (void)normalIdx;
    (void)mrIdx;
    (void)emissiveIdx;
    (void)type;
    return 0;
#else
    if (!materialBuffer_) return 0;

    // Reuses a slot freed by a material eviction before growing.
    uint32_t index;
    if (!freeMaterialIndices_.empty()) {
        index = freeMaterialIndices_.back();
        freeMaterialIndices_.pop_back();
    } else {
        index = nextMaterialIndex_++;
        if (index >= maxMaterials_) {
            Log::warn("Global material buffer full! Cap is ", maxMaterials_, ".");
            return 0; // fallback to 0
        }
    }

    writeMaterialSlot(index, baseColor, emissive, metallic, roughness, ao,
                      albedoIdx, normalIdx, mrIdx, emissiveIdx, type);
    return index;
#endif
}

void BindlessTables::writeMaterialSlot(uint32_t index, const glm::vec4& baseColor,
                                       const glm::vec4& emissive,
                                       float metallic, float roughness, float ao,
                                       uint32_t albedoIdx, uint32_t normalIdx, uint32_t mrIdx,
                                       uint32_t emissiveIdx, MaterialType type) {
#ifdef SAIDA_RHI_WEBGPU
    (void)index;
    (void)baseColor;
    (void)emissive;
    (void)metallic;
    (void)roughness;
    (void)ao;
    (void)albedoIdx;
    (void)normalIdx;
    (void)mrIdx;
    (void)emissiveIdx;
    (void)type;
#else
    if (!materialBuffer_) return;

    MaterialData data{};
    data.baseColor = baseColor;
    data.emissive = emissive;
    data.metallic = metallic;
    data.roughness = roughness;
    data.ao = ao;
    data.albedoTexIdx = albedoIdx;
    data.normalTexIdx = normalIdx;
    data.mrTexIdx = mrIdx;
    data.emissiveTexIdx = emissiveIdx;
    data.materialType = static_cast<uint32_t>(type);

    void* mapped = materialBuffer_->mapped();
    if (mapped) {
        const uint64_t offset = uint64_t(index) * sizeof(MaterialData);
        memcpy(static_cast<char*>(mapped) + offset, &data, sizeof(MaterialData));
        // VMA allocations may be non-coherent; publishing material data without
        // this flush made newly-created bindless materials platform-dependent.
        materialBuffer_->flush(sizeof(MaterialData), offset);
    }
#endif
}

} // namespace saida
