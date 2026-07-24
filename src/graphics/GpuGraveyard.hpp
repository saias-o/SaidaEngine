#pragma once

#include "rhi/Rhi.hpp"

#ifdef SAIDA_RHI_WEBGPU
// On WebGPU, Texture is a type alias (rhi::webgpu::Texture) — include it rather
// than forward-declare.
#include "graphics/Texture.hpp"
#endif

#include <cstdint>
#include <memory>
#include <vector>

namespace saida {

class BindlessTables;
class Mesh;
class Material;
#ifndef SAIDA_RHI_WEBGPU
class Texture;
#endif

// A GPU object retired from a live cache but possibly still read by a frame in
// flight. Destroyed — and its bindless texture index / material slot recycled —
// only after GpuGraveyard::kRetireFrames pumps, once no in-flight frame can
// sample it.
struct Retired {
    Retired();
    ~Retired();
    Retired(Retired&&) noexcept;
    Retired& operator=(Retired&&) noexcept;
    Retired(const Retired&) = delete;
    Retired& operator=(const Retired&) = delete;

    std::unique_ptr<Texture> texture;
    std::unique_ptr<Mesh> mesh;
    std::unique_ptr<Material> material;
    std::unique_ptr<rhi::BindGroup> bindGroup;
    uint32_t bindlessIndex = ~0u;
    uint32_t materialIndex = ~0u;
    uint64_t frame = 0;
};

// Owns retired GPU objects and destroys them on a delay that outlasts the
// frames-in-flight window, recycling their bindless table slots as they go.
// (The methods are out-of-line because ~Retired needs the complete resource
// types, which are aliases on WebGPU.)
class GpuGraveyard {
public:
    // Frames to keep a retired object alive; must exceed frames in flight (2).
    static constexpr uint64_t kRetireFrames = 4;

    GpuGraveyard();
    ~GpuGraveyard();
    GpuGraveyard(const GpuGraveyard&) = delete;
    GpuGraveyard& operator=(const GpuGraveyard&) = delete;

    // Retire an object, tagged with the current frame.
    void retire(Retired r, uint64_t frame);
    // Convenience for a lone bind group (e.g. a rebuilt material's descriptor).
    void retireBindGroup(std::unique_ptr<rhi::BindGroup> group, uint64_t frame);

    // Destroys entries older than kRetireFrames, recycling their table slots.
    // `defaultWhite` repoints freed texture indices (pass null when the tables
    // are inactive).
    void drain(uint64_t frameClock, BindlessTables& tables, Texture* defaultWhite);

    void clear();

private:
    std::vector<Retired> entries_;
};

} // namespace saida
