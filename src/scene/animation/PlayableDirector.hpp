#pragma once

#include "scene/Behaviour.hpp"
#include "scene/animation/Timeline.hpp"
#include <memory>

namespace saida {

// PlayableDirector is a Behaviour that controls the playback of a generic Timeline.
class PlayableDirector : public Behaviour {
public:
    void onUpdate(float dt) override;

    void play(std::shared_ptr<Timeline> timeline);
    void stop();
    void pause();
    void resume();

    const char* typeName() const override { return "PlayableDirector"; }

private:
    std::shared_ptr<Timeline> currentTimeline_;
    float time_ = 0.0f;
    float speed_ = 1.0f;
    bool isPlaying_ = false;
    bool looping_ = false;
};

} // namespace saida
