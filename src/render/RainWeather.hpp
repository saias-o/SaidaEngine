#pragma once

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace saida {

struct RainWeatherUniforms {
    glm::vec4 params{0.f}; // x intensity, y wetness, z seconds, w unused
    glm::vec4 up{0.f, 1.f, 0.f, 0.f};
};

inline RainWeatherUniforms packRainWeather(float intensity, float wetness,
                                          const glm::vec3& up, float seconds) {
    const auto unit = [](float value) {
        return std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 0.f;
    };
    RainWeatherUniforms result;
    result.params = {unit(intensity), unit(wetness),
                     std::isfinite(seconds) ? std::max(seconds, 0.f) : 0.f, 0.f};
    const float lengthSquared = glm::dot(up, up);
    if (std::isfinite(lengthSquared) && lengthSquared > 1e-12f)
        result.up = glm::vec4(up / std::sqrt(lengthSquared), 0.f);
    return result;
}

} // namespace saida
