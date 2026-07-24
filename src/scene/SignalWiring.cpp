#include "scene/SignalWiring.hpp"

#include "core/Log.hpp"
#include "core/Reflection.hpp"
#include "scene/Behaviour.hpp"
#include "scene/Node.hpp"

namespace saida {
namespace {

using nlohmann::json;

Node* findById(Node& root, NodeId id) {
    if (root.id() == id) return &root;
    for (const auto& child : root.children())
        if (Node* found = findById(*child, id)) return found;
    return nullptr;
}

// Locate a reflected signal `name` on a node itself or on one of its behaviours.
// Returns the object pointer to bind plus the descriptor (both null if missing).
struct SignalHit { void* obj = nullptr; const reflect::SignalDesc* desc = nullptr; };
SignalHit findSignal(Node& node, const std::string& name) {
    auto& reg = reflect::TypeRegistry::instance();
    if (const auto* d = reg.find(node.typeName()))
        if (const auto* s = d->findSignal(name)) return {&node, s};
    for (const auto& b : node.behaviours())
        if (const auto* d = reg.find(b->typeName() ? b->typeName() : ""))
            if (const auto* s = d->findSignal(name)) return {b.get(), s};
    return {};
}

struct SlotHit { void* obj = nullptr; const reflect::SlotDesc* desc = nullptr; };
SlotHit findSlot(Node& node, const std::string& name) {
    auto& reg = reflect::TypeRegistry::instance();
    if (const auto* d = reg.find(node.typeName()))
        if (const auto* s = d->findSlot(name)) return {&node, s};
    for (const auto& b : node.behaviours())
        if (const auto* d = reg.find(b->typeName() ? b->typeName() : ""))
            if (const auto* s = d->findSlot(name)) return {b.get(), s};
    return {};
}

} // namespace

void SignalWiring::apply(Node& root, const std::vector<SignalConnectionDef>& defs) {
    clear();
    for (const SignalConnectionDef& def : defs) {
        Node* from = findById(root, def.from);
        if (!from) {
            Log::warn("SignalWiring: emitter node ", def.from, " not found");
            continue;
        }
        SignalHit sig = findSignal(*from, def.signal);
        if (!sig.desc) {
            Log::warn("SignalWiring: signal '", def.signal, "' not found on node ", def.from);
            continue;
        }
        // Warn early if the target is missing or has no such slot — but still
        // bind by id, since both are re-resolved at emit time (correct across
        // reparenting/destruction).
        if (Node* target = findById(root, def.to)) {
            if (!findSlot(*target, def.slot).desc)
                Log::warn("SignalWiring: slot '", def.slot, "' not found on node ", def.to);
        } else {
            Log::warn("SignalWiring: target node ", def.to, " not found");
        }

        Node* rootPtr = &root;
        NodeId toId = def.to;
        std::string slotName = def.slot;
        connections_.push_back(sig.desc->connect(sig.obj, [rootPtr, toId, slotName](const json& args) {
            Node* target = findById(*rootPtr, toId);
            if (!target) return;  // target freed → silent no-op
            SlotHit slot = findSlot(*target, slotName);
            if (slot.desc) slot.desc->invoke(slot.obj, args);
        }));
    }
}

} // namespace saida
