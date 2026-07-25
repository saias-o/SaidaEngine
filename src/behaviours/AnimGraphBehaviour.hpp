#pragma once

#include "scene/Behaviour.hpp"
#include "core/Reflection.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace saida {

class Animator;

// Attaches a .sgraph to every Animator below this node, and nothing else.
//
// An Animator is built by the glTF import, not authored in a scene, so a scene
// cannot address one directly. Something authored has to reach down to it --
// which is what CharacterBehaviour does, bundled with its own movement logic. A
// game driving movement itself (scripts, another controller) still needs the
// graph half alone; that half is this behaviour.
//
// Invariant: the graph is applied at most once, and a failure is terminal and
// logged. A partially applied graph -- some Animators driven, others left on
// their play()-by-name FSM -- would desynchronise a character's limbs, so the
// first Animator that rejects the graph stops the whole attempt.
class AnimGraphBehaviour : public Behaviour {
public:
    AnimGraphBehaviour() = default;
    ~AnimGraphBehaviour() override = default;

    void onUpdate(float dt) override;

    SAIDA_REFLECT_BEHAVIOUR(AnimGraphBehaviour, "AnimGraph")

    // Project-relative .sgraph. Empty means this behaviour does nothing, which
    // leaves the Animators on their play()-by-name FSM.
    std::string graph;

    // True once the graph is compiled and driving every Animator below.
    bool applied() const { return applied_; }

private:
    std::vector<Animator*> animators_;
    bool searched_ = false;
    uint64_t assetId_ = 0;
    bool applied_ = false;
    bool failed_ = false;
};

} // namespace saida
