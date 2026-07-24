#include "core/InputGamepad.hpp"

#include <cassert>
#include <array>
#include <cmath>

namespace {

bool near(float actual, float expected) {
    return std::abs(actual - expected) < 0.0001f;
}

void testSignedStickDeadzone() {
    using saida::GamepadAxis;
    using saida::input_detail::gamepadAxisActionValue;

    assert(near(gamepadAxisActionValue(GamepadAxis::LeftX, 0.09f, 1.0f, 0.1f), 0.0f));
    assert(near(gamepadAxisActionValue(GamepadAxis::LeftX, 0.55f, 1.0f, 0.1f), 0.5f));
    assert(near(gamepadAxisActionValue(GamepadAxis::LeftX, -0.55f, 1.0f, 0.1f), 0.0f));
    assert(near(gamepadAxisActionValue(GamepadAxis::LeftX, -0.55f, -1.0f, 0.1f), 0.5f));
    assert(near(gamepadAxisActionValue(GamepadAxis::LeftX, 1.0f, 2.0f, 0.1f), 1.0f));
}

void testTriggerNormalization() {
    using saida::GamepadAxis;
    using saida::input_detail::gamepadAxisActionValue;

    assert(near(gamepadAxisActionValue(GamepadAxis::RightTrigger, -1.0f, 1.0f, 0.1f), 0.0f));
    assert(near(gamepadAxisActionValue(GamepadAxis::RightTrigger, 0.0f, 1.0f, 0.1f),
                4.0f / 9.0f));
    assert(near(gamepadAxisActionValue(GamepadAxis::RightTrigger, 1.0f, 1.0f, 0.1f), 1.0f));
}

void testDeadzoneBounds() {
    using saida::input_detail::applyDeadzone;

    assert(near(applyDeadzone(0.5f, -1.0f), 0.5f));
    assert(near(applyDeadzone(0.98f, 2.0f), 0.0f));
    assert(near(applyDeadzone(1.0f, 2.0f), 1.0f));
}

void testActivityIgnoresDrift() {
    using saida::GamepadAxis;
    using saida::input_detail::gamepadAxisHasActivity;

    assert(!gamepadAxisHasActivity(GamepadAxis::LeftX, 0.0f, 0.04f));
    assert(!gamepadAxisHasActivity(GamepadAxis::LeftX, 0.0f, 0.2f));
    assert(gamepadAxisHasActivity(GamepadAxis::LeftX, 0.0f, 0.4f));
    assert(gamepadAxisHasActivity(GamepadAxis::LeftX, 0.4f, 0.0f));
    assert(!gamepadAxisHasActivity(GamepadAxis::RightTrigger, -1.0f, -0.9f));
    assert(gamepadAxisHasActivity(GamepadAxis::RightTrigger, -1.0f, -0.5f));
    assert(gamepadAxisHasActivity(GamepadAxis::RightTrigger, 0.5f, -1.0f));
}

void testWebStandardMapping() {
    using namespace saida;
    using namespace saida::input_detail;

    std::array<double, 4> axes{1.5, -0.25, 0.5, -2.0};
    std::array<double, 17> analog{};
    std::array<bool, 17> digital{};
    analog[1] = 0.75;   // B through the analog threshold
    analog[6] = 0.25;   // left trigger => -0.5 in GLFW convention
    analog[7] = 1.0;    // right trigger => 1.0
    digital[0] = true;  // A
    digital[8] = true;  // Back
    digital[12] = true; // Dpad up
    digital[15] = true; // Dpad right
    digital[16] = true; // Guide

    StandardGamepadState state;
    assert(mapWebStandardGamepad(
        {axes.data(), int(axes.size()), analog.data(), digital.data(),
         int(analog.size())},
        state));

    assert(state.buttons[indexOf(GamepadButton::A)] == 1);
    assert(state.buttons[indexOf(GamepadButton::B)] == 1);
    assert(state.buttons[indexOf(GamepadButton::Back)] == 1);
    assert(state.buttons[indexOf(GamepadButton::DpadUp)] == 1);
    assert(state.buttons[indexOf(GamepadButton::DpadRight)] == 1);
    assert(state.buttons[indexOf(GamepadButton::Guide)] == 1);
    assert(state.buttons[indexOf(GamepadButton::DpadDown)] == 0);
    assert(near(state.axes[indexOf(GamepadAxis::LeftX)], 1.0f));
    assert(near(state.axes[indexOf(GamepadAxis::LeftY)], -0.25f));
    assert(near(state.axes[indexOf(GamepadAxis::RightX)], 0.5f));
    assert(near(state.axes[indexOf(GamepadAxis::RightY)], -1.0f));
    assert(near(state.axes[indexOf(GamepadAxis::LeftTrigger)], -0.5f));
    assert(near(state.axes[indexOf(GamepadAxis::RightTrigger)], 1.0f));
}

void testWebStandardMappingRejectsIncompleteState() {
    using namespace saida::input_detail;

    std::array<double, 4> axes{};
    std::array<double, 15> analog{};
    std::array<bool, 15> digital{};
    StandardGamepadState state;
    assert(!mapWebStandardGamepad(
        {axes.data(), int(axes.size()), analog.data(), digital.data(),
         int(analog.size())},
        state));
    assert(near(state.axes[4], -1.0f));
    assert(near(state.axes[5], -1.0f));
}

} // namespace

int main() {
    testSignedStickDeadzone();
    testTriggerNormalization();
    testDeadzoneBounds();
    testActivityIgnoresDrift();
    testWebStandardMapping();
    testWebStandardMappingRejectsIncompleteState();
    return 0;
}
