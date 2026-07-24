#pragma once

#include "core/InputEnums.hpp"
#include "core/InputLimits.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>

namespace saida::input_detail {

constexpr size_t kStandardGamepadButtonCount =
    static_cast<size_t>(GamepadButton::DpadLeft) + 1;
constexpr size_t kStandardGamepadAxisCount =
    static_cast<size_t>(GamepadAxis::RightTrigger) + 1;

// Semantic layout shared by the GLFW and browser backends. Keeping the browser
// conversion independent from Emscripten makes the mapping deterministic and
// testable in the native suite.
struct StandardGamepadState {
    std::array<uint8_t, kStandardGamepadButtonCount> buttons{};
    std::array<float, kStandardGamepadAxisCount> axes{0.0f, 0.0f, 0.0f, 0.0f,
                                                      -1.0f, -1.0f};
};

struct WebStandardGamepadSample {
    const double* axes = nullptr;
    int axisCount = 0;
    const double* analogButtons = nullptr;
    const bool* digitalButtons = nullptr;
    int buttonCount = 0;
};

inline size_t indexOf(GamepadButton button) {
    return static_cast<size_t>(button);
}

inline size_t indexOf(GamepadAxis axis) {
    return static_cast<size_t>(axis);
}

// W3C "standard" layout:
//   buttons 0..5 face/shoulders, 6..7 triggers, 8..16
//   back/start/sticks/dpad/guide; axes 0..3 are the two sticks.
// Triggers are converted from browser [0, 1] to GLFW [-1, 1].
inline bool mapWebStandardGamepad(const WebStandardGamepadSample& sample,
                                  StandardGamepadState& out) {
    out = {};
    out.axes[indexOf(GamepadAxis::LeftTrigger)] = -1.0f;
    out.axes[indexOf(GamepadAxis::RightTrigger)] = -1.0f;

    if (!sample.axes || !sample.analogButtons || !sample.digitalButtons ||
        sample.axisCount < 4 || sample.buttonCount < 16) {
        return false;
    }

    const auto pressed = [&](int browserButton) {
        return sample.digitalButtons[browserButton] ||
               sample.analogButtons[browserButton] > 0.5;
    };
    const auto setButton = [&](GamepadButton button, int browserButton) {
        out.buttons[indexOf(button)] = pressed(browserButton) ? uint8_t{1}
                                                              : uint8_t{0};
    };

    setButton(GamepadButton::A, 0);
    setButton(GamepadButton::B, 1);
    setButton(GamepadButton::X, 2);
    setButton(GamepadButton::Y, 3);
    setButton(GamepadButton::LeftBumper, 4);
    setButton(GamepadButton::RightBumper, 5);
    setButton(GamepadButton::Back, 8);
    setButton(GamepadButton::Start, 9);
    setButton(GamepadButton::LeftThumb, 10);
    setButton(GamepadButton::RightThumb, 11);
    setButton(GamepadButton::DpadUp, 12);
    setButton(GamepadButton::DpadDown, 13);
    setButton(GamepadButton::DpadLeft, 14);
    setButton(GamepadButton::DpadRight, 15);
    if (sample.buttonCount > 16) setButton(GamepadButton::Guide, 16);

    out.axes[indexOf(GamepadAxis::LeftX)] =
        std::clamp(static_cast<float>(sample.axes[0]), -1.0f, 1.0f);
    out.axes[indexOf(GamepadAxis::LeftY)] =
        std::clamp(static_cast<float>(sample.axes[1]), -1.0f, 1.0f);
    out.axes[indexOf(GamepadAxis::RightX)] =
        std::clamp(static_cast<float>(sample.axes[2]), -1.0f, 1.0f);
    out.axes[indexOf(GamepadAxis::RightY)] =
        std::clamp(static_cast<float>(sample.axes[3]), -1.0f, 1.0f);
    out.axes[indexOf(GamepadAxis::LeftTrigger)] =
        std::clamp(static_cast<float>(sample.analogButtons[6]) * 2.0f - 1.0f,
                   -1.0f, 1.0f);
    out.axes[indexOf(GamepadAxis::RightTrigger)] =
        std::clamp(static_cast<float>(sample.analogButtons[7]) * 2.0f - 1.0f,
                   -1.0f, 1.0f);
    return true;
}

inline float applyDeadzone(float value, float deadzone) {
    value = std::clamp(value, -1.0f, 1.0f);
    deadzone = std::clamp(deadzone, 0.0f, kMaxGamepadDeadzone);
    const float magnitude = std::abs(value);
    if (magnitude <= deadzone) return 0.0f;
    return std::copysign((magnitude - deadzone) / (1.0f - deadzone), value);
}

inline bool isTriggerAxis(GamepadAxis axis) {
    return axis == GamepadAxis::LeftTrigger || axis == GamepadAxis::RightTrigger;
}

// Analog drift at rest must not steal the last active device. An input
// counts when it crosses the threshold or returns from an active
// position, with a minimal delta to filter out noise.
inline bool gamepadAxisHasActivity(GamepadAxis axis, float previous,
                                   float current) {
    previous = std::clamp(previous, -1.0f, 1.0f);
    current = std::clamp(current, -1.0f, 1.0f);
    if (std::abs(current - previous) <= 0.05f) return false;
    if (isTriggerAxis(axis)) return std::max(previous, current) > -0.8f;
    return std::max(std::abs(previous), std::abs(current)) > 0.25f;
}

// GLFW reports sticks in [-1, 1] and triggers in [-1, 1], where -1 is
// released. Action bindings are directional strengths in [0, 1]; bind the
// opposite stick direction to another action with a negative scale.
inline float gamepadAxisActionValue(GamepadAxis axis, float rawValue, float scale,
                                    float deadzone) {
    float value = std::clamp(rawValue, -1.0f, 1.0f);
    if (isTriggerAxis(axis)) value = (value + 1.0f) * 0.5f;
    value = applyDeadzone(value, deadzone) * scale;
    return std::clamp(value, 0.0f, 1.0f);
}

} // namespace saida::input_detail
