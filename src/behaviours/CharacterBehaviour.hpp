#pragma once

#include "scene/Behaviour.hpp"
#include "core/Reflection.hpp"

#include <cstdint>
#include <string>

namespace saida {

class Animator;

class CharacterBehaviour : public Behaviour {
public:
    CharacterBehaviour() = default;
    ~CharacterBehaviour() override = default;

    void onReady() override;
    void onUpdate(float dt) override;

    SAIDA_REFLECT_BEHAVIOUR(CharacterBehaviour, "Character")

    float moveSpeed = 5.0f;
    float sprintMultiplier = 1.8f;  // speed factor while holding Sprint (Shift)
    float jumpForce = 5.0f;
    float gravity = 9.81f;

    // Movement is relative to the active camera (found via the "camera" group). The
    // character turns to face its movement direction when faceMovement is on.
    bool faceMovement = true;
    float turnSpeed = 12.0f;  // exponential turn rate toward the move direction

    // Animation clips played from a child Animator based on movement state. Empty
    // or missing names are simply skipped (animation is optional).
    std::string idleClip = "Idle";
    std::string walkClip = "Walk";
    std::string jumpClip = "Jump";

    // Optional .sgraph (project-relative path). If present, the graph's FSM
    // owns playback; this behaviour only feeds the "speed" parameter.
    std::string graph;

private:
    void updateAnimation(bool onFloor, bool moving);

    bool warned_ = false;     // warn once if attached to a non-CharacterBody node
    Animator* animator_ = nullptr;  // cached child Animator (found lazily)
    uint64_t graphAssetId_ = 0;
    bool graphApplied_ = false;
    bool graphFailed_ = false;
};

} // namespace saida
