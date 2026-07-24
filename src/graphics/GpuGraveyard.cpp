#include "graphics/GpuGraveyard.hpp"

#include "graphics/BindlessTables.hpp"
#include "graphics/Material.hpp"
#include "graphics/Mesh.hpp"
#include "graphics/Texture.hpp"

namespace saida {

Retired::Retired() = default;
Retired::~Retired() = default;
Retired::Retired(Retired&&) noexcept = default;
Retired& Retired::operator=(Retired&&) noexcept = default;

GpuGraveyard::GpuGraveyard() = default;
GpuGraveyard::~GpuGraveyard() = default;

void GpuGraveyard::retire(Retired r, uint64_t frame) {
    r.frame = frame;
    entries_.push_back(std::move(r));
}

void GpuGraveyard::retireBindGroup(std::unique_ptr<rhi::BindGroup> group, uint64_t frame) {
    if (!group) return;
    Retired r;
    r.bindGroup = std::move(group);
    retire(std::move(r), frame);
}

void GpuGraveyard::drain(uint64_t frameClock, BindlessTables& tables, Texture* defaultWhite) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (frameClock - it->frame < kRetireFrames) {
            ++it;
            continue;
        }
        // No in-flight frame references it: the bindless index repoints at the
        // default white texture then becomes allocatable again, the material
        // slot returns to the freelist, and the destructors free the GPU memory.
        if (it->bindlessIndex != ~0u)
            tables.recycleTextureIndex(it->bindlessIndex, defaultWhite);
        if (it->materialIndex != ~0u)
            tables.recycleMaterialSlot(it->materialIndex);
        it = entries_.erase(it);
    }
}

void GpuGraveyard::clear() { entries_.clear(); }

} // namespace saida
