// Fixed-step time and visual smoothing: the two pieces a host needs between
// its variable frame rate and the session's fixed simulation step.
//
// Header-only, standard library only. No clock is read here: the host passes
// elapsed time, so a replay or a test drives it exactly.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace saida::netcode {

// Turns elapsed frame time into a whole number of simulation ticks. The
// session's time scale (Session::timeScale) stretches or shrinks the step by a
// few percent so peers drift back into step without a visible stall. Catch-up
// is bounded: a hitch never asks for more than `maxTicks` ticks in one frame,
// the rest of the debt is dropped, not accumulated into a spiral.
class TickClock {
public:
    explicit TickClock(double tickSeconds = 1.0 / 60.0, int maxTicks = 4)
        : tick_(tickSeconds > 0.0 ? tickSeconds : 1.0 / 60.0), maxTicks_(std::max(1, maxTicks)) {}

    // Ticks to run for `frameSeconds` of real time.
    int advance(double frameSeconds, double timeScale = 1.0) {
        if (!std::isfinite(frameSeconds) || frameSeconds < 0.0) frameSeconds = 0.0;
        if (!std::isfinite(timeScale) || timeScale <= 0.0) timeScale = 1.0;
        accumulator_ += frameSeconds * timeScale;
        const double due = std::floor(accumulator_ / tick_);
        if (!(due <= double(maxTicks_))) {
            accumulator_ = 0.0;
            ++dropped_;
            return maxTicks_;
        }
        accumulator_ -= due * tick_;
        return int(due);
    }
    // How far the present lies between the last tick and the next, in [0, 1):
    // the render blend between the two most recent simulated states.
    double alpha() const { return std::clamp(accumulator_ / tick_, 0.0, 1.0); }
    double tickSeconds() const { return tick_; }
    // Frames on which catch-up debt was dropped.
    size_t dropped() const { return dropped_; }
    // Restarts the step, carrying `late` seconds already elapsed (a
    // synchronized start observed after its instant).
    void reset(double late = 0.0) { accumulator_ = std::isfinite(late) && late > 0.0 ? late : 0.0; }

private:
    double tick_;
    int maxTicks_;
    double accumulator_ = 0.0;
    size_t dropped_ = 0;
};

// Hides a rollback correction. Before a tick, the host notes where it was
// drawing an object; after the tick, where the corrected simulation puts it.
// The difference becomes an offset that decays to zero, so a remote player
// glides onto its corrected path instead of teleporting. The simulation is
// never touched: only the drawn value carries the offset. A correction larger
// than `snap` (a respawn, a teleport) is shown at once.
template <size_t N>
class Smoother {
public:
    explicit Smoother(float halfLife = 0.1f, float snap = 4.f) : halfLife_(halfLife), snap_(snap) {}

    void correct(const float (&drawn)[N], const float (&simulated)[N]) {
        float d2 = 0.f;
        float next[N];
        for (size_t i = 0; i < N; ++i) {
            next[i] = offset_[i] + drawn[i] - simulated[i];
            d2 += next[i] * next[i];
        }
        if (!(d2 <= snap_ * snap_)) return clear();
        for (size_t i = 0; i < N; ++i) offset_[i] = next[i];
    }
    void decay(float seconds) {
        if (halfLife_ <= 0.f) return clear();
        const float k = std::exp2(-std::max(0.f, seconds) / halfLife_);
        for (float& o : offset_) o *= k;
    }
    void apply(const float (&simulated)[N], float (&drawn)[N]) const {
        for (size_t i = 0; i < N; ++i) drawn[i] = simulated[i] + offset_[i];
    }
    void clear() {
        for (float& o : offset_) o = 0.f;
    }
    const float* offset() const { return offset_; }

private:
    float halfLife_, snap_;
    float offset_[N] = {};
};

} // namespace saida::netcode
