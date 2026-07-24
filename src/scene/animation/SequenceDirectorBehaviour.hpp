#pragma once

#include "core/Reflection.hpp"
#include "core/Signal.hpp"
#include "scene/Behaviour.hpp"
#include "scene/animation/AnimationSequence.hpp"

#include <string>
#include <vector>

namespace saida {

// Plays an AnimationSequence (.sseq) in a running scene. Track targets are
// resolved by name in the owning node's scene: an animation track drives the
// Animator of the target node (or a descendant), a property track
// "Name.property" drives a reflected property on the target node or one of
// its behaviours, and the event track is relayed through the reflected
// `sequenceEvent` signal. Fail-closed: an invalid sequence or a target still
// missing after the resolution delay disables playback with logged
// diagnostics, without emitting any signal.
class SequenceDirectorBehaviour : public Behaviour {
public:
    void onUpdate(float dt) override;

    SAIDA_REFLECT_BEHAVIOUR(SequenceDirectorBehaviour, "SequenceDirector")

    std::string sequence;  // project-relative path to the .sseq
    bool autoplay = true;

    void play();  // (re)starts from the beginning; binding happens on the next update
    void stop();

    Signal<std::string> sequenceEvent;  // relays the event track
    Signal<> sequenceFinished;          // end of playback (once per run)

private:
    enum class BindState { Unbound, Bound, Failed };

    bool tryBind(float dt);
    void failWith(const std::vector<AssetDiagnostic>& diags, const char* stage);

    SequencePlayer player_;
    BindState bindState_ = BindState::Unbound;
    float bindWait_ = 0.0f;
    bool playing_ = false;
    bool playRequested_ = false;
    bool autoplayConsumed_ = false;
    bool finishedEmitted_ = false;
};

} // namespace saida
