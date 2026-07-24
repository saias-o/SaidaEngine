#pragma once

namespace saida {

// Global frame timing, similar to Unity's `Time`. The Engine advances it once per
// frame; behaviours (and anything else) read it. `delta()` is scaled by
// `scale()` so setting the scale to 0 freezes gameplay (e.g. editor pause)
// while unscaled real time keeps flowing for the editor camera.
class Time {
public:
    static float delta() { return delta_; }            // scaled seconds since last frame
    static float unscaledDelta() { return unscaledDelta_; }
    static float elapsed() { return elapsed_; }        // scaled seconds since start
    static float scale() { return scale_; }
    static void setScale(float s) {
        scale_ = s;
        delta_ = unscaledDelta_ * scale_;
    }

    // Advance the clock. The desktop Engine calls this once per frame via its
    // own loop; a standalone frame host that does not use Engine (the web
    // runtime, which plays the Engine's role) calls it directly. Prefer letting
    // the Engine own timing wherever an Engine exists.
    static void advance(float realDeltaSeconds) { update(realDeltaSeconds); }

private:
    friend class Engine;
    static void update(float realDeltaSeconds) {
        unscaledDelta_ = realDeltaSeconds;
        delta_ = realDeltaSeconds * scale_;
        elapsed_ += delta_;
    }

    static inline float delta_ = 0.0f;
    static inline float unscaledDelta_ = 0.0f;
    static inline float elapsed_ = 0.0f;
    static inline float scale_ = 1.0f;
};

} // namespace saida
