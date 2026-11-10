#pragma once
#include "Wildlife.hpp"

namespace saida::wildlife {
// A ground companion follows a host-supplied point at a bounded speed. The
// host owns routing, tether anchors and the leader's wait policy. No teleport
// on a blocked step: wait for a clear route or retire with the leader.
class Companion {
public:
    Vec2 position;
    float heading = 0.f, speed = 0.f;
    float acceleration = 2.8f, braking = 4.5f, turnRate = 2.8f;
    float angularSpeed = 0.f;
    bool blocked = false;
    using Clear = std::function<bool(Vec2 from, Vec2 to)>;

    void update(float dt, Vec2 target, float pace, bool held, const Clear& clear, Vec2 targetVelocity = {}) {
        blocked = false;
        if (!(dt > 0.f)) return;
        if (held) { speed = angularSpeed = 0.f; return; }
        dt = std::min(dt, .1f);
        const Vec2 delta = target - position;
        const float gap = length(delta);
        const Vec2 wanted = targetVelocity + delta * 3.f;
        const float desiredLength = length(wanted);
        const float turn = desiredLength > .02f ? std::remainder(headingOf(wanted) - heading, 6.28318530718f) : 0.f;
        // Turn before advancing, so the body never slides sideways around a
        // corner. Tight turns slow it down instead of rotating at full pace.
        angularSpeed = std::clamp(turn * 7.f, -turnRate, turnRate);
        heading += angularSpeed * dt;
        const float arrival = length(targetVelocity) < .03f ? std::sqrt(2.f * braking * std::max(0.f, gap - .015f)) : pace;
        float desired = std::min({std::max(0.f, pace), desiredLength, arrival});
        desired *= std::max(0.f, std::cos(turn));
        const float change = (desired > speed ? acceleration : braking) * dt;
        speed += std::clamp(desired - speed, -change, change);
        if (speed < .015f && gap < .025f) { speed = angularSpeed = 0.f; return; }
        const Vec2 next = position + forward(heading) * std::min(speed * dt, std::max(gap, length(targetVelocity) * dt));
        if (clear && !clear(position, next)) { blocked = true; speed = angularSpeed = 0.f; return; }
        position = next;
    }
};
} // namespace saida::wildlife
