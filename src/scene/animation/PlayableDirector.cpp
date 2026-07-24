#include "scene/animation/PlayableDirector.hpp"
#include <cmath>

namespace saida {

void PlayableDirector::onUpdate(float dt) {
    if (!isPlaying_ || !currentTimeline_) return;

    time_ += dt * speed_;
    float duration = currentTimeline_->duration();

    if (duration > 0.0f) {
        if (time_ > duration) {
            if (looping_) {
                time_ = std::fmod(time_, duration);
            } else {
                time_ = duration;
                isPlaying_ = false; // Auto-stop at the end
            }
        }
    }

    currentTimeline_->evaluate(time_);
}

void PlayableDirector::play(std::shared_ptr<Timeline> timeline) {
    currentTimeline_ = std::move(timeline);
    time_ = 0.0f;
    isPlaying_ = true;
}

void PlayableDirector::stop() {
    isPlaying_ = false;
    time_ = 0.0f;
    if (currentTimeline_) {
        currentTimeline_->evaluate(0.0f); // Reset state
    }
}

void PlayableDirector::pause() {
    isPlaying_ = false;
}

void PlayableDirector::resume() {
    if (currentTimeline_) {
        isPlaying_ = true;
    }
}

} // namespace saida
