#pragma once

#include "core/Input.hpp"

#include <algorithm>
#include <cmath>

namespace saida::input_detail {

inline TouchGesture classifyTouchGesture(glm::vec2 start, glm::vec2 end,
                                         float tapMaxDistance = 24.0f) {
    const glm::vec2 delta = end - start;
    if (glm::dot(delta, delta) <= tapMaxDistance * tapMaxDistance)
        return TouchGesture::Tap;
    if (std::abs(delta.x) >= std::abs(delta.y))
        return delta.x >= 0.0f ? TouchGesture::SwipeRight
                               : TouchGesture::SwipeLeft;
    return delta.y >= 0.0f ? TouchGesture::SwipeDown
                           : TouchGesture::SwipeUp;
}

inline bool touchPointInZone(glm::vec2 point, glm::vec2 viewport,
                             glm::vec2 zoneMin, glm::vec2 zoneMax) {
    if (viewport.x <= 0.0f || viewport.y <= 0.0f) return false;
    const glm::vec2 normalized{point.x / viewport.x, point.y / viewport.y};
    return normalized.x >= zoneMin.x && normalized.y >= zoneMin.y &&
           normalized.x <= zoneMax.x && normalized.y <= zoneMax.y;
}

inline bool validTouchZone(glm::vec2 zoneMin, glm::vec2 zoneMax) {
    return zoneMin.x >= 0.0f && zoneMin.y >= 0.0f &&
           zoneMax.x <= 1.0f && zoneMax.y <= 1.0f &&
           zoneMin.x <= zoneMax.x && zoneMin.y <= zoneMax.y;
}

} // namespace saida::input_detail
