#pragma once

#include "core/Signal.hpp"
#include "scene/NodeId.hpp"

#include <string>
#include <vector>

namespace saida {

class Node;

// One data-driven signal→slot link, as stored in a scene's "connections" block.
struct SignalConnectionDef {
    NodeId from = kNodeInvalid;
    std::string signal;          // reflected signal name on the `from` node/behaviour
    NodeId to = kNodeInvalid;
    std::string slot;            // reflected slot name on the `to` node/behaviour
};

// Data-driven reflected signal -> slot wiring. Targets are re-resolved by NodeId
// at emit time, so freed targets become no-ops.
class SignalWiring {
public:
    // (Re)wire every def against the subtree rooted at `root`. Clears any prior
    // connections first, so calling twice is safe.
    void apply(Node& root, const std::vector<SignalConnectionDef>& defs);
    void clear() { connections_.clear(); }
    std::size_t count() const { return connections_.size(); }

private:
    std::vector<Connection> connections_;
};

} // namespace saida
