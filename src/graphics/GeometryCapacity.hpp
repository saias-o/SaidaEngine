#pragma once

#include <cstdint>

namespace saida {

// Fixed GPU arenas are chosen before device resources are created. Games may
// select a capacity without recompiling the engine and query the same contract.
struct GeometryCapacity {
    uint64_t vertices = 1024 * 1024;
    uint64_t indices = 3 * 1024 * 1024;
};

} // namespace saida
