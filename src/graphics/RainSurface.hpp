#pragma once

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace saida {

// Runtime material response to the scene's rainfall and retained surface water.
// Reception is opt-in; the other parameters have no effect when it is zero.
struct RainSurface {
    float reception = 0.f;
    float puddleAmount = .35f;
    float rippleStrength = .18f;
    float wetDarkening = .20f;

    bool operator==(const RainSurface& other) const {
        return reception == other.reception && puddleAmount == other.puddleAmount &&
               rippleStrength == other.rippleStrength && wetDarkening == other.wetDarkening;
    }
    glm::vec4 parameters() const {
        const auto unit = [](float value) {
            return std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 0.f;
        };
        return {unit(reception), unit(puddleAmount), unit(rippleStrength), unit(wetDarkening)};
    }
};

} // namespace saida
