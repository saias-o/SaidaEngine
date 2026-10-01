#pragma once

#include <cstdint>

namespace saida {

// Jolt sizes its body table, body-pair cache and contact buffers once, when a
// physics world is built, and none of them grows. Games choose them at startup
// without recompiling the engine and query the same contract.
struct PhysicsCapacity {
    // Every body alive at once: static, kinematic, dynamic, sensors and each
    // character's inner body. A compound shape is one body however many
    // primitives it holds.
    uint32_t bodies = 8192;
    // Broadphase pairs examined per step, and contact constraints solved per
    // step. A step that overflows either drops contacts (said once per kind).
    uint32_t bodyPairs = 8192;
    uint32_t contactConstraints = 4096;

    // Jolt keeps 23 bits of a BodyID for the body index.
    static constexpr uint32_t kMaxBodies = 0x7fffff;

    bool valid() const {
        return bodies > 0 && bodies <= kMaxBodies && bodyPairs > 0 && contactConstraints > 0;
    }
};

} // namespace saida
