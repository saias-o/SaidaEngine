#pragma once

#include <cstdint>

namespace saida {

using NodeId = uint64_t;
constexpr NodeId kNodeInvalid = 0;

NodeId generateNodeId();

} // namespace saida
