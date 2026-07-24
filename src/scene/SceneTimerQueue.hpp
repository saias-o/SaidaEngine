#pragma once

#include "core/Easing.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace saida {

class Behaviour;
class Node;

using TimerId = uint64_t;
inline constexpr TimerId kInvalidTimerId = 0;

// Runtime timer storage shared by every SceneTree backend. Keeping scheduling
// separate from scene loading makes the timing contract independently testable.
class SceneTimerQueue {
public:
    TimerId after(Node* owner, float seconds, std::function<void()> fn);
    TimerId every(Node* owner, float interval, std::function<void()> fn);
    TimerId tween(Node* owner, float duration, Easing easing,
                  std::function<void(float)> fn);

    TimerId after(Behaviour* owner, float seconds, std::function<void()> fn);
    TimerId every(Behaviour* owner, float interval, std::function<void()> fn);
    TimerId tween(Behaviour* owner, float duration, Easing easing,
                  std::function<void(float)> fn);

    void cancel(TimerId id);
    void cancelOwnedBy(Node* owner);
    void cancelOwnedBy(Behaviour* owner);
    void clear();
    void tick(float dt);

private:
    enum class Kind { After, Every, Tween };

    struct Timer {
        TimerId id = kInvalidTimerId;
        Node* nodeOwner = nullptr;
        Behaviour* behaviourOwner = nullptr;
        Kind kind = Kind::After;
        float elapsed = 0.0f;
        float duration = 0.0f;
        Easing easing = Easing::Linear;
        std::function<void()> callback;
        std::function<void(float)> tweenCallback;
        bool cancelled = false;
    };

    TimerId add(Timer timer);

    std::vector<Timer> timers_;
    TimerId nextId_ = kInvalidTimerId;
};

} // namespace saida
