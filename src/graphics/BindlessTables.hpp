#pragma once

#ifndef SAIDA_RHI_WEBGPU
#include <vulkan/vulkan.h>
#endif

#include "graphics/Material.hpp"
#include "rhi/Rhi.hpp"

#ifdef SAIDA_RHI_WEBGPU
// On WebGPU, Buffer/Texture are type aliases (rhi::webgpu::*) — they cannot be
// forward-declared as classes.
#include "graphics/Buffer.hpp"
#include "graphics/Texture.hpp"
#endif

#include <memory>
#include <vector>

namespace saida {

#ifndef SAIDA_RHI_WEBGPU
class Buffer;
class Texture;
#endif

// The global bindless descriptor tables shared by every pipeline: one Vulkan
// descriptor set with binding 0 = the bindless texture array and binding 1 =
// the MaterialData SSBO, plus the index/slot allocation and recycling for both.
//
// Invariants: texture index 0 and material slot 0 are shared fallbacks and are
// never recycled; a recycled texture index is repointed at the default white
// texture before going back to the freelist, so in-flight frames never sample a
// destroyed image. On WebGPU the tables are inert: every index resolves to 0.
class BindlessTables {
public:
    BindlessTables(rhi::Device& device, uint32_t maxTextures, uint32_t maxMaterials);
    ~BindlessTables();
    BindlessTables(const BindlessTables&) = delete;
    BindlessTables& operator=(const BindlessTables&) = delete;

    // Creates layout/pool/set and the MaterialData SSBO. No-op on WebGPU or when
    // the device lacks descriptor indexing (the tables stay inactive).
    void create();

    // True when the descriptor set exists (the bindless path is usable).
    bool active() const;

    // --- texture facet ------------------------------------------------------
    // Registers the texture in the bindless array if needed and returns its
    // index (0 when inactive). The index is cached on the texture itself.
    uint32_t ensureTextureIndex(Texture* texture);
    // Returns the index to the freelist; when `defaultWhite` is non-null the
    // slot is repointed at it first so in-flight frames keep sampling something
    // valid.
    void recycleTextureIndex(uint32_t index, Texture* defaultWhite);

    // --- material facet -----------------------------------------------------
    // Allocates a slot (reusing recycled ones first) and writes it. Returns the
    // slot index, 0 on overflow or when inactive.
    uint32_t allocMaterialSlot(const glm::vec4& baseColor, const glm::vec4& emissive,
                               float metallic, float roughness, float ao,
                               uint32_t albedoIdx, uint32_t normalIdx, uint32_t mrIdx,
                               uint32_t emissiveIdx, MaterialType type);
    // Rewrites an already-allocated slot (material rebind after an async load).
    void writeMaterialSlot(uint32_t index, const glm::vec4& baseColor,
                           const glm::vec4& emissive,
                           float metallic, float roughness, float ao,
                           uint32_t albedoIdx, uint32_t normalIdx, uint32_t mrIdx,
                           uint32_t emissiveIdx, MaterialType type);
    void recycleMaterialSlot(uint32_t index) { freeMaterialIndices_.push_back(index); }

    // --- accessors ----------------------------------------------------------
#ifndef SAIDA_RHI_WEBGPU
    VkDescriptorSetLayout layout() const { return layout_; }
    VkDescriptorSet set() const { return set_; }
#endif
    Buffer* materialBuffer() const { return materialBuffer_.get(); }

private:
    uint32_t allocTextureIndex(Texture* texture);

    rhi::Device& device_;
    const uint32_t maxTextures_;
    const uint32_t maxMaterials_;
#ifndef SAIDA_RHI_WEBGPU
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
#endif
    std::unique_ptr<Buffer> materialBuffer_;
    uint32_t nextTextureIndex_ = 0;
    uint32_t nextMaterialIndex_ = 0;
    std::vector<uint32_t> freeTextureIndices_;
    std::vector<uint32_t> freeMaterialIndices_;
};

} // namespace saida
