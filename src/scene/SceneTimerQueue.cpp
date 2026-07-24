#include "scene/SceneTimerQueue.hpp"

#include <algorithm>
#include <utility>

namespace saida {

namespace {

float nonNegative(float value) { return std::max(value, 0.0f); }

} // namespace

TimerId SceneTimerQueue::add(Timer timer) {
    timer.id = ++nextId_;
    timers_.push_back(std::move(timer));
    return timers_.back().id;
}

TimerId SceneTimerQueue::after(Node* owner, float seconds, std::function<void()> fn) {
    return add({kInvalidTimerId, owner, nullptr, Kind::After, 0.0f,
                nonNegative(seconds), Easing::Linear, std::move(fn), {}, false});
}

TimerId SceneTimerQueue::every(Node* owner, float interval, std::function<void()> fn) {
    if (interval <= 0.0f) return kInvalidTimerId;
    return add({kInvalidTimerId, owner, nullptr, Kind::Every, 0.0f,
                interval, Easing::Linear, std::move(fn), {}, false});
}

TimerId SceneTimerQueue::tween(Node* owner, float duration, Easing easing,
                               std::function<void(float)> fn) {
    return add({kInvalidTimerId, owner, nullptr, Kind::Tween, 0.0f,
                nonNegative(duration), easing, {}, std::move(fn), false});
}

TimerId SceneTimerQueue::after(Behaviour* owner, float seconds, std::function<void()> fn) {
    return add({kInvalidTimerId, nullptr, owner, Kind::After, 0.0f,
                nonNegative(seconds), Easing::Linear, std::move(fn), {}, false});
}

TimerId SceneTimerQueue::every(Behaviour* owner, float interval, std::function<void()> fn) {
    if (interval <= 0.0f) return kInvalidTimerId;
    return add({kInvalidTimerId, nullptr, owner, Kind::Every, 0.0f,
                interval, Easing::Linear, std::move(fn), {}, false});
}

TimerId SceneTimerQueue::tween(Behaviour* owner, float duration, Easing easing,
                               std::function<void(float)> fn) {
    return add({kInvalidTimerId, nullptr, owner, Kind::Tween, 0.0f,
                nonNegative(duration), easing, {}, std::move(fn), false});
}

void SceneTimerQueue::cancel(TimerId id) {
    if (id == kInvalidTimerId) return;
    for (auto& timer : timers_) {
        if (timer.id == id) {
            timer.cancelled = true;
            return;
        }
    }
}

void SceneTimerQueue::cancelOwnedBy(Node* owner) {
    for (auto& timer : timers_)
        if (timer.nodeOwner == owner) timer.cancelled = true;
}

void SceneTimerQueue::cancelOwnedBy(Behaviour* owner) {
    for (auto& timer : timers_)
        if (timer.behaviourOwner == owner) timer.cancelled = true;
}

void SceneTimerQueue::clear() { timers_.clear(); }

void SceneTimerQueue::tick(float dt) {
    if (dt <= 0.0f) return;

    // Callbacks may append timers. Only timers present at tick start execute;
    // copied callables remain valid if vector growth reallocates the storage.
    const size_t initialCount = timers_.size();
    for (size_t i = 0; i < initialCount; ++i) {
        if (timers_[i].cancelled) continue;
        timers_[i].elapsed += dt;

        if (timers_[i].kind == Kind::Tween) {
            const float progress = timers_[i].duration > 0.0f
                ? timers_[i].elapsed / timers_[i].duration
                : 1.0f;
            const bool complete = progress >= 1.0f;
            const float eased = applyEasing(timers_[i].easing,
                                             complete ? 1.0f : progress);
            auto callback = timers_[i].tweenCallback;
            if (complete) timers_[i].cancelled = true;
            if (callback) callback(eased);
            continue;
        }

        if (timers_[i].elapsed < timers_[i].duration) continue;
        auto callback = timers_[i].callback;
        if (timers_[i].kind == Kind::After)
            timers_[i].cancelled = true;
        else
            timers_[i].elapsed -= timers_[i].duration;
        if (callback) callback();
    }

    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                 [](const Timer& timer) { return timer.cancelled; }),
                  timers_.end());
}

} // namespace saida
