#pragma once

#include <cstdint>

namespace saida {

// Fixed GPU arenas are chosen before device resources are created. Games may
// select a capacity without recompiling the engine and query the same contract.
struct GeometryCapacity {
    uint64_t vertices = 1024 * 1024;
    uint64_t indices = 3 * 1024 * 1024;
};

// What the arenas hold now. The largest free ranges bound the biggest mesh
// that can still be uploaded: free space split in pieces is not one piece.
struct GeometryUsage {
    uint64_t vertices = 0;
    uint64_t indices = 0;
    uint64_t largestFreeVertices = 0;
    uint64_t largestFreeIndices = 0;
    uint32_t allocations = 0;
};

} // namespace saida
