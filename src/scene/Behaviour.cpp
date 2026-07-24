#include "scene/Behaviour.hpp"

#include "scene/Node.hpp"
#include "scene/SceneTimerQueue.hpp"
#ifdef SAIDA_SCENE_TREE_TIMERS
#include "scene/SceneTree.hpp"
#endif

namespace saida {

Behaviour::~Behaviour() { cancelTimers(); }

SceneTree* Behaviour::tree() const { return node_ ? node_->tree() : nullptr; }

uint64_t Behaviour::wait(float seconds, std::function<void()> fn) {
#ifndef SAIDA_SCENE_TREE_TIMERS
    (void)seconds;
    (void)fn;
    return kInvalidTimerId;
#else
    SceneTree* t = tree();
    if (!t) return kInvalidTimerId;
    timerTree_ = t;
    return t->after(this, seconds, std::move(fn));
#endif
}

uint64_t Behaviour::every(float interval, std::function<void()> fn) {
#ifndef SAIDA_SCENE_TREE_TIMERS
    (void)interval;
    (void)fn;
    return kInvalidTimerId;
#else
    SceneTree* t = tree();
    if (!t) return kInvalidTimerId;
    timerTree_ = t;
    return t->every(this, interval, std::move(fn));
#endif
}

uint64_t Behaviour::tween(float duration, Easing easing, std::function<void(float)> fn) {
#ifndef SAIDA_SCENE_TREE_TIMERS
    (void)duration;
    (void)easing;
    (void)fn;
    return kInvalidTimerId;
#else
    SceneTree* t = tree();
    if (!t) return kInvalidTimerId;
    timerTree_ = t;
    return t->tween(this, duration, easing, std::move(fn));
#endif
}

void Behaviour::cancelTimer(uint64_t id) {
#ifdef SAIDA_SCENE_TREE_TIMERS
    if (timerTree_) timerTree_->cancelTimer(id);
#else
    (void)id;
#endif
}

void Behaviour::cancelTimers() {
#ifdef SAIDA_SCENE_TREE_TIMERS
    if (timerTree_) timerTree_->cancelTimersOwnedBy(this);
#endif
    timerTree_ = nullptr;
}

} // namespace saida
