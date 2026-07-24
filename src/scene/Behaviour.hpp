#pragma once

#include "nlohmann/json_fwd.hpp"  // forward decl only; full header in .cpp
#include "core/Signal.hpp"
#include "core/Easing.hpp"

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace saida {

class Node;
class SceneTree;

// A piece of logic attached to a Node (cf. Unity MonoBehaviour / Godot script).
// The owning node calls onReady() once before the first update, then onUpdate()
// every frame. A behaviour can reach its node (and thus its transform, children,
// siblings) via node().
//
// To make a behaviour serializable, override typeName() (a stable id), save()
// and load(), and register a factory with the BehaviourRegistry. Behaviours
// without a typeName are skipped on save.
class Behaviour {
public:
    virtual ~Behaviour();
    virtual void onReady() {}
    virtual void onUpdate(float /*dt*/) {}

    // Lifecycle hooks. onDestroy fires when the node is being destroyed (while it
    // is still valid). onEnable/onDisable fire when enabled flips after onReady.
    virtual void onDestroy() {}
    virtual void onEnable() {}
    virtual void onDisable() {}

    virtual const char* typeName() const { return nullptr; }
    // Runtime-only implementation behaviours (trackers, adapters) can stay out
    // of the authoring UI without pretending to be serializable components.
    virtual bool visibleInEditor() const { return true; }
    virtual void save(nlohmann::json&) const {}
    virtual void load(const nlohmann::json&) {}

    Node* node() const { return node_; }
    SceneTree* tree() const;  // shortcut for node()->tree()

    bool enabled() const { return enabled_; }
    void setEnabled(bool enabled) {
        if (enabled_ == enabled) return;
        enabled_ = enabled;
        if (ready_) { if (enabled_) onEnable(); else onDisable(); }
    }

    // Subscribe to a signal; the connection lives as long as THIS behaviour
    // (auto-disconnected on destruction). The clean way to react to events —
    // "signal up", never grab another node by name.
    template <typename... Args, typename Fn>
    void listen(const Signal<Args...>& sig, Fn&& fn) {
        connections_.push_back(sig.connect(std::forward<Fn>(fn)));
    }

    // Timers/tweens scoped to this behaviour's node (cancelled when it dies).
    uint64_t wait(float seconds, std::function<void()> fn);   // one-shot
    uint64_t every(float interval, std::function<void()> fn); // repeating
    uint64_t tween(float duration, Easing easing, std::function<void(float)> fn);
    void cancelTimer(uint64_t id);

private:
    friend class Node;  // sets node_ on attach, drives ready_/lifecycle
    friend class Scene; // flattens behaviours and drives lifecycle
    Node* node_ = nullptr;
    void cancelTimers();
    bool ready_ = false;
    bool enabled_ = true;
    std::vector<Connection> connections_;  // owned subscriptions
    SceneTree* timerTree_ = nullptr;
};

} // namespace saida
