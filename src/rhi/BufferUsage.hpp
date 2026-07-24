#pragma once

#include <cstdint>

namespace saida::rhi {

enum class BufferUsage : uint32_t {
    None        = 0,
    Vertex      = 1u << 0,
    Index       = 1u << 1,
    Uniform     = 1u << 2,
    Storage     = 1u << 3,
    Indirect    = 1u << 4,
    TransferSrc = 1u << 5,  // copy source (staging upload)
    TransferDst = 1u << 6,  // copy destination (device-local target)
};

inline BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool hasFlag(BufferUsage set, BufferUsage flag) {
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(flag)) != 0u;
}

} // namespace saida::rhi
