#include "scene/NodeId.hpp"

#include <atomic>
#include <chrono>
#include <random>

namespace saida {

NodeId generateNodeId() {
    static std::atomic<NodeId> next([] {
        std::random_device rd;
        NodeId seed = (static_cast<NodeId>(rd()) << 32) ^ static_cast<NodeId>(rd());
        seed ^= static_cast<NodeId>(std::chrono::high_resolution_clock::now()
                                        .time_since_epoch().count());
        return seed > 1 ? seed : NodeId{2};
    }());
    NodeId id = next.fetch_add(1, std::memory_order_relaxed);
    return id == kNodeInvalid ? next.fetch_add(1, std::memory_order_relaxed) : id;
}

} // namespace saida
