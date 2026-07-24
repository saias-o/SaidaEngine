#pragma once

#include <cmath>

namespace saida {

// Easing curves for tweens. `applyEasing` maps a normalized time t in [0,1] to an
// eased value (also in [0,1] for the in/out variants).
enum class Easing {
    Linear,
    InQuad,
    OutQuad,
    InOutQuad,
    OutBack,   // slight overshoot at the end (snappy UI feel)
};

inline float applyEasing(Easing e, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    switch (e) {
        case Easing::InQuad: return t * t;
        case Easing::OutQuad: return 1.0f - (1.0f - t) * (1.0f - t);
        case Easing::InOutQuad:
            return t < 0.5f ? 2.0f * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) / 2.0f;
        case Easing::OutBack: {
            const float c1 = 1.70158f, c3 = c1 + 1.0f;
            return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
        }
        case Easing::Linear:
        default:
            return t;
    }
}

} // namespace saida
