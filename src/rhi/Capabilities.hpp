#pragma once

#include <cstdint>

// Capabilities are API-neutral so the renderer need not know the active backend.

namespace saida {

// Quality tier derived from GPU capabilities.
enum class QualityTier {
    Low,     // mobile / integrated / UMA: no hardware RT, cheapest GI path
    Medium,  // console-class / mid GPU
    High,    // discrete desktop GPU
    Ultra,   // high-end desktop with hardware ray tracing
};

inline const char* toString(QualityTier t) {
    switch (t) {
        case QualityTier::Low:    return "Low";
        case QualityTier::Medium: return "Medium";
        case QualityTier::High:   return "High";
        case QualityTier::Ultra:  return "Ultra";
    }
    return "?";
}

namespace rhi {

// Names describe capabilities rather than API-specific extensions.
struct Capabilities {
    uint32_t apiVersion = 0;         // backend-defined (Vulkan: negotiated device version)

    bool dynamicRendering = false;   // render without explicit render-pass objects
    bool synchronization2 = false;   // fine-grained pipeline barriers / submit
    bool timelineSemaphore = false;  // graphics <-> async-compute sync
    bool descriptorIndexing = false; // bindless texture/material arrays
    bool bufferDeviceAddress = false;// GPU pointers (GPU-driven, BVH)
    bool drawIndirectCount = false;  // indirect draw with a GPU-provided count
    bool multiview = false;          // single-pass stereo (VR)
    bool multiDrawIndirect = false;  // indirect draw with drawCount > 1

    bool rayQuery = false;           // optional hardware ray tracing

    bool dedicatedComputeQueue = false; // a compute-only queue → true async compute
    bool discreteGpu = false;

    uint32_t maxSamples = 1;         // capped MSAA sample count (power of two)
    QualityTier tier = QualityTier::Low;
};

} // namespace rhi
} // namespace saida
