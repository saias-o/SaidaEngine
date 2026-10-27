#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "core/Input.hpp"

#include <cstdlib>
#include <iostream>

namespace {
int buttonLevels[GLFW_MOUSE_BUTTON_LAST + 1]{};
GLFWkeyfun keyCallback = nullptr;
bool lockKeyMods = false;
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "[input-consumption] FAIL: " << message << '\n';
        std::exit(1);
    }
}
}

// Drive the real sampler without hardware or a desktop session (Linux CI).
// These names replace only the test target's GLFW sampling/callback calls.
struct GLFWwindow {};
extern "C" int saidaTestMouseButton(GLFWwindow*, int button) {
    return buttonLevels[button];
}
extern "C" int saidaTestKey(GLFWwindow*, int) { return GLFW_RELEASE; }
extern "C" void saidaTestCursorPos(GLFWwindow*, double* x, double* y) { *x = *y = 0.; }
extern "C" void saidaTestWindowSize(GLFWwindow*, int* w, int* h) { *w = *h = 64; }
extern "C" GLFWcursorposfun saidaTestCursorCallback(GLFWwindow*, GLFWcursorposfun) { return nullptr; }
extern "C" GLFWscrollfun saidaTestScrollCallback(GLFWwindow*, GLFWscrollfun) { return nullptr; }
extern "C" GLFWcharfun saidaTestCharCallback(GLFWwindow*, GLFWcharfun) { return nullptr; }
extern "C" GLFWkeyfun saidaTestKeyCallback(GLFWwindow*, GLFWkeyfun callback) {
    keyCallback = callback;
    return nullptr;
}
extern "C" void saidaTestInputMode(GLFWwindow*, int mode, int value) {
    if (mode == GLFW_LOCK_KEY_MODS) lockKeyMods = value == GLFW_TRUE;
}
extern "C" int saidaTestJoystickIsGamepad(int) { return GLFW_FALSE; }

int main() {
    using namespace saida;
    GLFWwindow window;
    Input::bindRaw(&window);
    Input::sample();

    for (const auto button : {MouseButton::Left, MouseButton::Right, MouseButton::Middle}) {
        const int index = button == MouseButton::Left ? GLFW_MOUSE_BUTTON_LEFT
                        : button == MouseButton::Right ? GLFW_MOUSE_BUTTON_RIGHT
                        : GLFW_MOUSE_BUTTON_MIDDLE;
        for (int gesture = 0; gesture < 3; ++gesture) {
            buttonLevels[index] = GLFW_PRESS;
            Input::sample();
            require(Input::isMouseButtonPressed(button), "each new gesture has one press");
            for (int frame = 0; frame < 5; ++frame) {
                Input::consumeMouse();
                require(!Input::isMouseButtonDown(button) &&
                        !Input::isMouseButtonPressed(button) &&
                        !Input::isMouseButtonReleased(button), "consumed buttons are hidden from gameplay");
                require(!Input::isActionHeld("Fire") && !Input::isActionHeld("Aim"),
                        "consumption masks mouse actions");
                Input::sample();
                require(Input::isMouseButtonDown(button), "UI sees the held hardware button next frame");
                require(!Input::isMouseButtonPressed(button), "holding over consumed UI does not repeat presses");
            }
            Input::consumeMouse();
            buttonLevels[index] = GLFW_RELEASE;
            Input::sample();
            require(Input::isMouseButtonReleased(button), "release reaches UI after a consumed hold");
            Input::consumeMouse();
            Input::sample();
            require(!Input::isMouseButtonReleased(button), "release is delivered once");
        }
    }

    // Lock keys are a state, not a held key: Num Lock on with its key up is
    // what keeps numpad digits typing digits in a text field.
    require(lockKeyMods && keyCallback, "the raw window asks GLFW for the lock modifiers");
    require(!Input::isNumLockOn() && !Input::isCapsLockOn(), "no lock reported before a key event");
    keyCallback(&window, GLFW_KEY_KP_8, 0, GLFW_PRESS, GLFW_MOD_NUM_LOCK);
    Input::sample();
    require(Input::isNumLockOn() && !Input::isKeyDown(KeyCode::NumLock), "Num Lock on while its key is up");
    require(!Input::isCapsLockOn(), "Num Lock alone leaves Caps Lock off");
    keyCallback(&window, GLFW_KEY_A, 0, GLFW_RELEASE, GLFW_MOD_CAPS_LOCK | GLFW_MOD_SHIFT);
    Input::sample();
    require(Input::isCapsLockOn() && !Input::isNumLockOn(), "the latest key event carries the lock state");
    keyCallback(&window, GLFW_KEY_A, 0, GLFW_PRESS, 0);
    Input::sample();
    require(!Input::isCapsLockOn() && !Input::isNumLockOn(), "locks turned off are reported off");

    Input::bindRaw(nullptr);
    std::cout << "[input-consumption] PASS (" << checks << " checks)\n";
}
