#pragma once

// Shared input tuning limits. Bindings and backend normalization use the same
// values, so serialized profiles cannot bypass runtime clamping.

namespace saida::input_detail {

inline constexpr float kDefaultGamepadDeadzone = 0.1f;
inline constexpr float kMaxGamepadDeadzone = 0.99f;
inline constexpr float kMaxTouchGestureDistancePixels = 4096.0f;

} // namespace saida::input_detail
