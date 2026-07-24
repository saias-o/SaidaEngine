#pragma once

#include "scene/Behaviour.hpp"
#include "core/Reflection.hpp"
#include "core/Signal.hpp"

namespace saida {

class Node;

// Generic hit points + death (a core gameplay component, like Godot/Unity ship).
// Other behaviours, hazards, or the data-driven signal wiring deal damage via the
// `damage` slot or finish it instantly with `kill`. On death it emits `died` (for
// scoring, drops, etc.), stops the node's other behaviours, optionally tips it
// over for readable feedback, and frees the node after `deathDelay` seconds.
class HealthBehaviour : public Behaviour {
public:
    void onReady() override;

    SAIDA_REFLECT_BEHAVIOUR(HealthBehaviour, "Health")

    void damage(float amount);   // reduce health; dies at <= 0
    void kill();                 // instant death
    bool dead() const { return dead_; }
    float health() const { return health_; }

    Signal<> died;  // emitted once, just before the death sequence

    float maxHealth = 100.0f;
    float deathDelay = 1.5f;       // seconds the corpse lingers before queueFree
    bool tipOverOnDeath = true;    // lie the body flat so a kill is obvious

private:
    void die();

    float health_ = 0.0f;
    bool dead_ = false;
};

// Convenience: deal `amount` damage to whatever Health is on `target`. No-op (and
// returns false) if the node is null, has no Health, or is already dead. The
// uniform way for weapons / cars / traps / falls to hurt something.
bool applyDamage(Node* target, float amount);

} // namespace saida
