#pragma once

#include "scene/Behaviour.hpp"
#include "core/Reflection.hpp"

#include <glm/glm.hpp>

namespace saida {

// Spins its node continuously around a local axis — the demo behaviour driving
// the orbiting "moon" in the sample scenes. Reflected: `axis`/`speed` properties,
// a `fullRotation` signal (one pulse per turn) and a `reset` slot. save()/load()
// and the API manifest are generated from describe().
class RotatorBehaviour : public Behaviour {
public:
    void onUpdate(float dt) override;

    // Slot: snap the node's rotation back to identity (wireable from data/JS).
    void reset();

    // Signal: emitted once each time the node completes a full 360° turn.
    Signal<> fullRotation;

    SAIDA_REFLECT_BEHAVIOUR(RotatorBehaviour, "Rotator")

    glm::vec3 axis{0.0f, 1.0f, 0.0f};  // local rotation axis
    float speed = 70.0f;               // degrees per second

private:
    float accumDegrees_ = 0.0f;        // toward the next fullRotation pulse
};

} // namespace saida
