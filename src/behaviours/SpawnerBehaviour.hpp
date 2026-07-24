#pragma once

#include "core/Reflection.hpp"
#include "scene/Behaviour.hpp"
#include <string>

namespace saida {

// Spawns a .scene instance at this node's position on a timer — a reusable demo
// of the gameplay primitives (instantiate + timers + lifetime/queueFree). Point
// `scenePath` at a project-relative .scene (e.g. "scenes/ball.scene"); give that
// scene a RigidBody to watch instances fall.
class SpawnerBehaviour : public Behaviour {
public:
    void onReady() override;

    std::string scenePath;     // project-relative .scene to instantiate
    float interval = 1.0f;     // seconds between spawns (<= 0 disables)
    float lifetime = 0.0f;     // seconds before a spawned instance is freed (0 = forever)

    SAIDA_REFLECT_BEHAVIOUR(SpawnerBehaviour, "Spawner")

private:
    void spawn();
};

} // namespace saida
