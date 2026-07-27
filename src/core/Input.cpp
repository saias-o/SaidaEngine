#include "core/Input.hpp"
#include "core/InputGamepad.hpp"
#include "core/InputProfile.hpp"
#include "core/InputTouch.hpp"
#include "core/Log.hpp"
#include "core/PlatformCaps.hpp"

// On web there is no Window wrapper (it forces GLFW_INCLUDE_VULKAN):
// only the bindRaw path is compiled, g_window always stays null.
#ifdef __EMSCRIPTEN__
#include <GLFW/glfw3.h>
#include <emscripten.h>
#include <emscripten/html5.h>
namespace saida { class Window; }
#else
#include "core/Window.hpp"
#endif

#include <algorithm>
#include <cstring>

namespace saida {

namespace {
Window* g_window = nullptr;

// Raw path (bindRaw): GLFW window without the Window wrapper; deltas are
// accumulated by callbacks and consumed each newFrame, just like Window does.
GLFWwindow* g_rawWindow = nullptr;
double g_rawMouseDx = 0.0, g_rawMouseDy = 0.0;
bool g_rawHasLastPos = false;
double g_rawLastX = 0.0, g_rawLastY = 0.0;
double g_rawScrollDx = 0.0, g_rawScrollDy = 0.0;
std::vector<uint32_t> g_rawTextInput;

// Raw hardware state
bool g_keyCurr[GLFW_KEY_LAST + 1] = {};
bool g_keyPrev[GLFW_KEY_LAST + 1] = {};
bool g_mouseCurr[GLFW_MOUSE_BUTTON_LAST + 1] = {};
bool g_mousePrev[GLFW_MOUSE_BUTTON_LAST + 1] = {};
glm::vec2 g_mouseDelta{0.0f};
glm::vec2 g_mousePos{0.0f};
glm::vec2 g_scrollDelta{0.0f};
std::vector<uint32_t> g_textInput;
std::vector<TouchPoint> g_touches;
std::vector<TouchPoint> g_pendingTouches;
std::vector<TouchGestureEvent> g_touchGestures;
std::vector<TouchGestureEvent> g_pendingTouchGestures;
std::unordered_map<uint64_t, glm::vec2> g_lastTouchPositions;
std::unordered_map<uint64_t, glm::vec2> g_touchStartPositions;
std::unordered_map<uint64_t, glm::vec2> g_activeTouchPositions;
glm::vec2 g_inputViewportSize{1.0f};
GLFWgamepadstate g_gamepadState{};
GLFWgamepadstate g_gamepadPrevState{};
int g_activeGamepad = -1;
int g_previousActiveGamepad = -1;
std::string g_activeGamepadName;
InputDevice g_lastActiveDevice = InputDevice::None;
#ifdef __EMSCRIPTEN__
bool g_touchBackendAvailable = false;

EM_JS(int, saida_web_gamepad_rumble,
      (int index, double lowFrequency, double highFrequency, int durationMs), {
    try {
        if (!navigator.getGamepads) return 0;
        const gamepad = navigator.getGamepads()[index];
        if (!gamepad) return 0;
        const actuator = gamepad.vibrationActuator;
        if (!actuator || typeof actuator.playEffect !== 'function') return 0;
        if (actuator.effects &&
            typeof actuator.effects.includes === 'function' &&
            !actuator.effects.includes('dual-rumble')) return 0;
        Promise.resolve(actuator.playEffect('dual-rumble', {
            startDelay: 0,
            duration: durationMs,
            strongMagnitude: lowFrequency,
            weakMagnitude: highFrequency
        })).catch(() => {});
        return 1;
    } catch (_) {
        return 0;
    }
});

EM_JS(int, saida_web_gamepad_stop_rumble, (int index), {
    try {
        if (!navigator.getGamepads) return 0;
        const gamepad = navigator.getGamepads()[index];
        if (!gamepad) return 0;
        const actuator = gamepad.vibrationActuator;
        if (!actuator || typeof actuator.reset !== 'function') return 0;
        Promise.resolve(actuator.reset()).catch(() => {});
        return 1;
    } catch (_) {
        return 0;
    }
});
#endif

// Injected virtual actions (tests/CI): combined with bindings via max.
struct InjectedAction {
    float target = 0.0f;
    float current = 0.0f;
    float previous = 0.0f;
};
std::unordered_map<std::string, InjectedAction> g_injectedActions;
bool g_uiCapturesKeyboard = false;
bool g_uiCapturesMouse = false;

// Action bindings & state
std::vector<ActionBinding> g_bindings;
std::vector<InputContextID> g_contextStack = { kGlobalContext };

struct Subscription {
    uint32_t id;
    std::string actionName;
    InputEvent eventType;
    ActionCallback callback;
    InputContextID context;
};
std::vector<Subscription> g_subscriptions;
uint32_t g_nextSubscriptionId = 1;

bool isContextActive(const InputContextID& context) {
    return std::find(g_contextStack.begin(), g_contextStack.end(), context) !=
           g_contextStack.end();
}

float aggregateActionStrength(const std::string& action, bool previous,
                              const InputContextID* onlyContext = nullptr) {
    float strength = 0.0f;
    for (const auto& binding : g_bindings) {
        if (binding.actionName != action || !isContextActive(binding.context)) continue;
        if (onlyContext && binding.context != *onlyContext) continue;
        strength = std::max(strength,
                            previous ? binding.previousValue : binding.currentValue);
    }
    if (!onlyContext || *onlyContext == kGlobalContext) {
        if (auto it = g_injectedActions.find(action); it != g_injectedActions.end())
            strength = std::max(strength, previous ? it->second.previous
                                                   : it->second.current);
    }
    return strength;
}

// Mappings
int glfwKeyFromKeyCode(KeyCode key) {
    if (key == KeyCode::Unknown) return GLFW_KEY_UNKNOWN;
    if (key >= KeyCode::Space && key <= KeyCode::GraveAccent) {
        // ASCII based mappings
        switch (key) {
            case KeyCode::Space: return GLFW_KEY_SPACE;
            case KeyCode::Apostrophe: return GLFW_KEY_APOSTROPHE;
            case KeyCode::Comma: return GLFW_KEY_COMMA;
            case KeyCode::Minus: return GLFW_KEY_MINUS;
            case KeyCode::Period: return GLFW_KEY_PERIOD;
            case KeyCode::Slash: return GLFW_KEY_SLASH;
            case KeyCode::Num0: return GLFW_KEY_0;
            case KeyCode::Num1: return GLFW_KEY_1;
            case KeyCode::Num2: return GLFW_KEY_2;
            case KeyCode::Num3: return GLFW_KEY_3;
            case KeyCode::Num4: return GLFW_KEY_4;
            case KeyCode::Num5: return GLFW_KEY_5;
            case KeyCode::Num6: return GLFW_KEY_6;
            case KeyCode::Num7: return GLFW_KEY_7;
            case KeyCode::Num8: return GLFW_KEY_8;
            case KeyCode::Num9: return GLFW_KEY_9;
            case KeyCode::Semicolon: return GLFW_KEY_SEMICOLON;
            case KeyCode::Equal: return GLFW_KEY_EQUAL;
            case KeyCode::A: return GLFW_KEY_A;
            case KeyCode::B: return GLFW_KEY_B;
            case KeyCode::C: return GLFW_KEY_C;
            case KeyCode::D: return GLFW_KEY_D;
            case KeyCode::E: return GLFW_KEY_E;
            case KeyCode::F: return GLFW_KEY_F;
            case KeyCode::G: return GLFW_KEY_G;
            case KeyCode::H: return GLFW_KEY_H;
            case KeyCode::I: return GLFW_KEY_I;
            case KeyCode::J: return GLFW_KEY_J;
            case KeyCode::K: return GLFW_KEY_K;
            case KeyCode::L: return GLFW_KEY_L;
            case KeyCode::M: return GLFW_KEY_M;
            case KeyCode::N: return GLFW_KEY_N;
            case KeyCode::O: return GLFW_KEY_O;
            case KeyCode::P: return GLFW_KEY_P;
            case KeyCode::Q: return GLFW_KEY_Q;
            case KeyCode::R: return GLFW_KEY_R;
            case KeyCode::S: return GLFW_KEY_S;
            case KeyCode::T: return GLFW_KEY_T;
            case KeyCode::U: return GLFW_KEY_U;
            case KeyCode::V: return GLFW_KEY_V;
            case KeyCode::W: return GLFW_KEY_W;
            case KeyCode::X: return GLFW_KEY_X;
            case KeyCode::Y: return GLFW_KEY_Y;
            case KeyCode::Z: return GLFW_KEY_Z;
            case KeyCode::LeftBracket: return GLFW_KEY_LEFT_BRACKET;
            case KeyCode::Backslash: return GLFW_KEY_BACKSLASH;
            case KeyCode::RightBracket: return GLFW_KEY_RIGHT_BRACKET;
            case KeyCode::GraveAccent: return GLFW_KEY_GRAVE_ACCENT;
            default: return GLFW_KEY_UNKNOWN;
        }
    }
    
    switch (key) {
        case KeyCode::Escape: return GLFW_KEY_ESCAPE;
        case KeyCode::Enter: return GLFW_KEY_ENTER;
        case KeyCode::Tab: return GLFW_KEY_TAB;
        case KeyCode::Backspace: return GLFW_KEY_BACKSPACE;
        case KeyCode::Insert: return GLFW_KEY_INSERT;
        case KeyCode::Delete: return GLFW_KEY_DELETE;
        case KeyCode::Right: return GLFW_KEY_RIGHT;
        case KeyCode::Left: return GLFW_KEY_LEFT;
        case KeyCode::Down: return GLFW_KEY_DOWN;
        case KeyCode::Up: return GLFW_KEY_UP;
        case KeyCode::PageUp: return GLFW_KEY_PAGE_UP;
        case KeyCode::PageDown: return GLFW_KEY_PAGE_DOWN;
        case KeyCode::Home: return GLFW_KEY_HOME;
        case KeyCode::End: return GLFW_KEY_END;
        case KeyCode::LeftShift: return GLFW_KEY_LEFT_SHIFT;
        case KeyCode::LeftControl: return GLFW_KEY_LEFT_CONTROL;
        case KeyCode::LeftAlt: return GLFW_KEY_LEFT_ALT;
        case KeyCode::RightShift: return GLFW_KEY_RIGHT_SHIFT;
        case KeyCode::RightControl: return GLFW_KEY_RIGHT_CONTROL;
        case KeyCode::RightAlt: return GLFW_KEY_RIGHT_ALT;
        default: return GLFW_KEY_UNKNOWN;
    }
}

int glfwMouseFromMouseButton(MouseButton btn) {
    return static_cast<int>(btn);
}

int glfwButtonFromGamepadButton(GamepadButton button) {
    switch (button) {
        case GamepadButton::A: return GLFW_GAMEPAD_BUTTON_A;
        case GamepadButton::B: return GLFW_GAMEPAD_BUTTON_B;
        case GamepadButton::X: return GLFW_GAMEPAD_BUTTON_X;
        case GamepadButton::Y: return GLFW_GAMEPAD_BUTTON_Y;
        case GamepadButton::LeftBumper: return GLFW_GAMEPAD_BUTTON_LEFT_BUMPER;
        case GamepadButton::RightBumper: return GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER;
        case GamepadButton::Back: return GLFW_GAMEPAD_BUTTON_BACK;
        case GamepadButton::Start: return GLFW_GAMEPAD_BUTTON_START;
        case GamepadButton::Guide: return GLFW_GAMEPAD_BUTTON_GUIDE;
        case GamepadButton::LeftThumb: return GLFW_GAMEPAD_BUTTON_LEFT_THUMB;
        case GamepadButton::RightThumb: return GLFW_GAMEPAD_BUTTON_RIGHT_THUMB;
        case GamepadButton::DpadUp: return GLFW_GAMEPAD_BUTTON_DPAD_UP;
        case GamepadButton::DpadRight: return GLFW_GAMEPAD_BUTTON_DPAD_RIGHT;
        case GamepadButton::DpadDown: return GLFW_GAMEPAD_BUTTON_DPAD_DOWN;
        case GamepadButton::DpadLeft: return GLFW_GAMEPAD_BUTTON_DPAD_LEFT;
    }
    return -1;
}

int glfwAxisFromGamepadAxis(GamepadAxis axis) {
    switch (axis) {
        case GamepadAxis::LeftX: return GLFW_GAMEPAD_AXIS_LEFT_X;
        case GamepadAxis::LeftY: return GLFW_GAMEPAD_AXIS_LEFT_Y;
        case GamepadAxis::RightX: return GLFW_GAMEPAD_AXIS_RIGHT_X;
        case GamepadAxis::RightY: return GLFW_GAMEPAD_AXIS_RIGHT_Y;
        case GamepadAxis::LeftTrigger: return GLFW_GAMEPAD_AXIS_LEFT_TRIGGER;
        case GamepadAxis::RightTrigger: return GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER;
    }
    return -1;
}

void sampleGamepad() {
    g_previousActiveGamepad = g_activeGamepad;
    g_gamepadPrevState = g_gamepadState;
    std::fill(std::begin(g_gamepadState.buttons), std::end(g_gamepadState.buttons),
              GLFW_RELEASE);
    std::fill(std::begin(g_gamepadState.axes), std::end(g_gamepadState.axes), 0.0f);

#ifdef __EMSCRIPTEN__
    int detected = -1;
    std::string detectedName;
    if (platform::has(platform::Capability::GamepadInput) &&
        emscripten_sample_gamepad_data() == EMSCRIPTEN_RESULT_SUCCESS) {
        const int gamepadCount = emscripten_get_num_gamepads();
        for (int slot = 0; slot < gamepadCount; ++slot) {
            EmscriptenGamepadEvent browserState{};
            if (emscripten_get_gamepad_status(slot, &browserState) !=
                    EMSCRIPTEN_RESULT_SUCCESS ||
                !browserState.connected ||
                std::strcmp(browserState.mapping, "standard") != 0) {
                continue;
            }

            input_detail::StandardGamepadState normalized;
            const input_detail::WebStandardGamepadSample sample{
                browserState.axis, browserState.numAxes,
                browserState.analogButton, browserState.digitalButton,
                browserState.numButtons};
            if (!input_detail::mapWebStandardGamepad(sample, normalized)) continue;

            detected = browserState.index;
            detectedName = browserState.id;
            for (size_t i = 0; i < normalized.buttons.size(); ++i)
                g_gamepadState.buttons[i] =
                    normalized.buttons[i] ? GLFW_PRESS : GLFW_RELEASE;
            for (size_t i = 0; i < normalized.axes.size(); ++i)
                g_gamepadState.axes[i] = normalized.axes[i];
            break;
        }
    }
#else
    int detected = -1;
    if (platform::has(platform::Capability::GamepadInput)) {
        for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
            if (glfwJoystickIsGamepad(jid) == GLFW_TRUE) {
                detected = jid;
                break;
            }
        }
    }

    if (detected >= 0 && glfwGetGamepadState(detected, &g_gamepadState) != GLFW_TRUE)
        detected = -1;
#endif

    if (detected == g_activeGamepad) return;
    if (g_activeGamepad >= 0)
        Log::info("gamepad disconnected: ", g_activeGamepadName);

    g_activeGamepad = detected;
    g_activeGamepadName.clear();
    if (g_activeGamepad >= 0) {
#ifdef __EMSCRIPTEN__
        g_activeGamepadName = std::move(detectedName);
#else
        if (const char* name = glfwGetGamepadName(g_activeGamepad))
            g_activeGamepadName = name;
#endif
        if (g_activeGamepadName.empty()) g_activeGamepadName = "Standard gamepad";
        Log::info("gamepad connected: ", g_activeGamepadName,
                  " (id=", g_activeGamepad, ")");
    }
}

bool gamepadHadActivity() {
    // Hotplug alone does not constitute a user action.
    if (g_activeGamepad < 0 || g_activeGamepad != g_previousActiveGamepad)
        return false;
    for (int i = 0; i <= GLFW_GAMEPAD_BUTTON_LAST; ++i) {
        if (g_gamepadState.buttons[i] != g_gamepadPrevState.buttons[i])
            return true;
    }
    for (int i = 0; i <= GLFW_GAMEPAD_AXIS_LAST; ++i) {
        if (input_detail::gamepadAxisHasActivity(
                static_cast<GamepadAxis>(i), g_gamepadPrevState.axes[i],
                g_gamepadState.axes[i])) {
            return true;
        }
    }
    return false;
}

#ifdef __EMSCRIPTEN__
EM_BOOL webTouchCallback(int eventType, const EmscriptenTouchEvent* event,
                         void*) {
    if (!event) return EM_FALSE;
    TouchPhase phase = TouchPhase::Moved;
    switch (eventType) {
        case EMSCRIPTEN_EVENT_TOUCHSTART: phase = TouchPhase::Began; break;
        case EMSCRIPTEN_EVENT_TOUCHEND: phase = TouchPhase::Ended; break;
        case EMSCRIPTEN_EVENT_TOUCHCANCEL: phase = TouchPhase::Cancelled; break;
        case EMSCRIPTEN_EVENT_TOUCHMOVE: phase = TouchPhase::Moved; break;
        default: return EM_FALSE;
    }
    for (int i = 0; i < event->numTouches; ++i) {
        const EmscriptenTouchPoint& point = event->touches[i];
        if (!point.isChanged) continue;
        Input::submitTouch(
            static_cast<uint64_t>(point.identifier),
            {static_cast<float>(point.targetX), static_cast<float>(point.targetY)},
            phase);
    }
    // The player's canvas owns the gesture: preventing browser scroll/zoom
    // stops a gameplay drag from being consumed by the page.
    return EM_TRUE;
}

bool installWebTouchBackend() {
    constexpr const char* kCanvas = "#canvas";
    const EMSCRIPTEN_RESULT start =
        emscripten_set_touchstart_callback(kCanvas, nullptr, false,
                                           webTouchCallback);
    const EMSCRIPTEN_RESULT move =
        emscripten_set_touchmove_callback(kCanvas, nullptr, false,
                                          webTouchCallback);
    const EMSCRIPTEN_RESULT end =
        emscripten_set_touchend_callback(kCanvas, nullptr, false,
                                         webTouchCallback);
    const EMSCRIPTEN_RESULT cancel =
        emscripten_set_touchcancel_callback(kCanvas, nullptr, false,
                                            webTouchCallback);
    return start == EMSCRIPTEN_RESULT_SUCCESS &&
           move == EMSCRIPTEN_RESULT_SUCCESS &&
           end == EMSCRIPTEN_RESULT_SUCCESS &&
           cancel == EMSCRIPTEN_RESULT_SUCCESS;
}
#endif

} // namespace

namespace {

void installDefaultBindings() {
    Input::bindKey("MoveForward", KeyCode::W);
    Input::bindKey("MoveBackward", KeyCode::S);
    Input::bindKey("MoveLeft", KeyCode::A);
    Input::bindKey("MoveRight", KeyCode::D);
    Input::bindKey("MoveUp", KeyCode::E);
    Input::bindKey("MoveDown", KeyCode::Q);

    Input::bindKey("Jump", KeyCode::Space);
    Input::bindKey("Sprint", KeyCode::LeftShift);

    Input::bindMouse("Fire", MouseButton::Left);
    Input::bindMouse("Aim", MouseButton::Right);

    Input::bindGamepadAxis("MoveForward", GamepadAxis::LeftY, -1.0f);
    Input::bindGamepadAxis("MoveBackward", GamepadAxis::LeftY, 1.0f);
    Input::bindGamepadAxis("MoveLeft", GamepadAxis::LeftX, -1.0f);
    Input::bindGamepadAxis("MoveRight", GamepadAxis::LeftX, 1.0f);
    // Right stick = look. Nothing consumed these before CameraFollow did, so
    // they are new names rather than a change to an existing binding.
    Input::bindGamepadAxis("LookLeft", GamepadAxis::RightX, -1.0f);
    Input::bindGamepadAxis("LookRight", GamepadAxis::RightX, 1.0f);
    Input::bindGamepadAxis("LookUp", GamepadAxis::RightY, -1.0f);
    Input::bindGamepadAxis("LookDown", GamepadAxis::RightY, 1.0f);
    Input::bindGamepadButton("Jump", GamepadButton::A);
    Input::bindGamepadButton("Sprint", GamepadButton::LeftThumb);
    Input::bindGamepadAxis("Fire", GamepadAxis::RightTrigger);
    Input::bindGamepadAxis("Aim", GamepadAxis::LeftTrigger);
}

} // namespace

void Input::bind(Window* window) {
    g_window = window;
    g_rawWindow = nullptr;
    installDefaultBindings();
}

void Input::bindRaw(GLFWwindow* window) {
    g_window = nullptr;
    g_rawWindow = window;
    g_rawHasLastPos = false;
    g_rawMouseDx = g_rawMouseDy = 0.0;
    g_rawScrollDx = g_rawScrollDy = 0.0;
    g_rawTextInput.clear();

    if (!window) return;

    glfwSetCursorPosCallback(window, [](GLFWwindow*, double x, double y) {
        if (g_rawHasLastPos) {
            g_rawMouseDx += x - g_rawLastX;
            g_rawMouseDy += y - g_rawLastY;
        }
        g_rawLastX = x;
        g_rawLastY = y;
        g_rawHasLastPos = true;
    });
    glfwSetScrollCallback(window, [](GLFWwindow*, double dx, double dy) {
        g_rawScrollDx += dx;
        g_rawScrollDy += dy;
    });
    glfwSetCharCallback(window, [](GLFWwindow*, unsigned int codepoint) {
        g_rawTextInput.push_back(codepoint);
    });

#ifdef __EMSCRIPTEN__
    g_touchBackendAvailable = installWebTouchBackend();
#endif
    installDefaultBindings();
}

float Input::evaluateBinding(const ActionBinding& binding) {
    if (!g_window && !g_rawWindow) return 0.0f;

    // Without the Window wrapper (raw player), no UI captures input.
#ifdef __EMSCRIPTEN__
    const bool ignoreUi = true;
#else
    const bool ignoreUi = g_window ? g_window->cursorCaptured() : true;
#endif

    if (binding.isKey) {
        if (g_uiCapturesKeyboard && !ignoreUi) return 0.0f;
        int glfwKey = glfwKeyFromKeyCode(binding.key);
        if (glfwKey >= 0 && glfwKey <= GLFW_KEY_LAST) {
            return g_keyCurr[glfwKey] ? 1.0f : 0.0f;
        }
    } else if (binding.isMouse) {
        if (g_uiCapturesMouse && !ignoreUi) return 0.0f;
        int glfwBtn = glfwMouseFromMouseButton(binding.mouseBtn);
        if (glfwBtn >= 0 && glfwBtn <= GLFW_MOUSE_BUTTON_LAST) {
            return g_mouseCurr[glfwBtn] ? 1.0f : 0.0f;
        }
    } else if (binding.isGamepadBtn) {
        const int button = glfwButtonFromGamepadButton(binding.padBtn);
        if (g_activeGamepad >= 0 && button >= 0 && button <= GLFW_GAMEPAD_BUTTON_LAST)
            return g_gamepadState.buttons[button] == GLFW_PRESS ? 1.0f : 0.0f;
    } else if (binding.isGamepadAxis) {
        const int axis = glfwAxisFromGamepadAxis(binding.padAxis);
        if (g_activeGamepad >= 0 && axis >= 0 && axis <= GLFW_GAMEPAD_AXIS_LAST) {
            return input_detail::gamepadAxisActionValue(
                binding.padAxis, g_gamepadState.axes[axis], binding.axisScale,
                binding.deadzone);
        }
    } else if (binding.isTouch) {
        if (!platform::has(platform::Capability::TouchInput)) return 0.0f;
        if (binding.touchGesture == TouchGesture::Press) {
            for (const auto& [id, position] : g_activeTouchPositions) {
                (void)id;
                if (input_detail::touchPointInZone(
                        position, g_inputViewportSize,
                        binding.touchZoneMin, binding.touchZoneMax)) {
                    return 1.0f;
                }
            }
        } else {
            for (const TouchGestureEvent& gesture : g_touchGestures) {
                if (gesture.gesture != binding.touchGesture ||
                    !input_detail::touchPointInZone(
                        gesture.startPosition, g_inputViewportSize,
                        binding.touchZoneMin, binding.touchZoneMax)) {
                    continue;
                }
                if (binding.touchGesture == TouchGesture::Tap ||
                    gesture.distance >= binding.touchMinDistance) {
                    return 1.0f;
                }
            }
        }
    }

    return 0.0f;
}

bool Input::uiCapturesKeyboard() { return g_uiCapturesKeyboard; }
bool Input::uiCapturesMouse() { return g_uiCapturesMouse; }

void Input::setUiCapture(bool keyboard, bool mouse) {
    g_uiCapturesKeyboard = keyboard;
    g_uiCapturesMouse = mouse;
}

void Input::newFrame() {
    if (!g_window && !g_rawWindow) return;
#ifdef __EMSCRIPTEN__
    GLFWwindow* w = g_rawWindow;
#else
    GLFWwindow* w = g_window ? g_window->handle() : g_rawWindow;
#endif

    // 1. Sample raw hardware state
    for (int k = 0; k <= GLFW_KEY_LAST; ++k) {
        g_keyPrev[k] = g_keyCurr[k];
        g_keyCurr[k] = glfwGetKey(w, k) == GLFW_PRESS;
    }
    for (int b = 0; b <= GLFW_MOUSE_BUTTON_LAST; ++b) {
        g_mousePrev[b] = g_mouseCurr[b];
        g_mouseCurr[b] = glfwGetMouseButton(w, b) == GLFW_PRESS;
    }
    sampleGamepad();

#ifndef __EMSCRIPTEN__
    if (g_window) {
        double dx, dy;
        g_window->consumeMouseDelta(dx, dy);
        g_mouseDelta = {static_cast<float>(dx), static_cast<float>(dy)};

        double sx, sy;
        g_window->consumeScrollDelta(sx, sy);
        g_scrollDelta = {static_cast<float>(sx), static_cast<float>(sy)};
        g_textInput = g_window->consumeTextInput();
    } else
#endif
    {
        g_mouseDelta = {static_cast<float>(g_rawMouseDx), static_cast<float>(g_rawMouseDy)};
        g_rawMouseDx = g_rawMouseDy = 0.0;
        g_scrollDelta = {static_cast<float>(g_rawScrollDx), static_cast<float>(g_rawScrollDy)};
        g_rawScrollDx = g_rawScrollDy = 0.0;
        g_textInput = std::move(g_rawTextInput);
        g_rawTextInput.clear();
    }
    g_touches = std::move(g_pendingTouches);
    g_pendingTouches.clear();
    g_touchGestures = std::move(g_pendingTouchGestures);
    g_pendingTouchGestures.clear();

    double mx, my;
    glfwGetCursorPos(w, &mx, &my);
    g_mousePos = {static_cast<float>(mx), static_cast<float>(my)};
    int viewportWidth = 0;
    int viewportHeight = 0;
    glfwGetWindowSize(w, &viewportWidth, &viewportHeight);
    g_inputViewportSize = {
        static_cast<float>(std::max(viewportWidth, 1)),
        static_cast<float>(std::max(viewportHeight, 1))};

    // Transitions (not held states) preserve a true recency order between
    // devices. A held keyboard key therefore doesn't steal prompts back from
    // a gamepad that just moved.
    bool keyboardMouseActivity =
        g_mouseDelta != glm::vec2(0.0f) || g_scrollDelta != glm::vec2(0.0f) ||
        !g_textInput.empty();
    for (int k = 0; !keyboardMouseActivity && k <= GLFW_KEY_LAST; ++k)
        keyboardMouseActivity = g_keyCurr[k] != g_keyPrev[k];
    for (int b = 0; !keyboardMouseActivity && b <= GLFW_MOUSE_BUTTON_LAST; ++b)
        keyboardMouseActivity = g_mouseCurr[b] != g_mousePrev[b];
    if (keyboardMouseActivity) g_lastActiveDevice = InputDevice::KeyboardMouse;
    if (gamepadHadActivity()) g_lastActiveDevice = InputDevice::Gamepad;
    if (!g_touches.empty()) g_lastActiveDevice = InputDevice::Touch;

    // 2. Advance injected action states (same current/previous cycle as bindings)
    for (auto& [name, injected] : g_injectedActions) {
        injected.previous = injected.current;
        injected.current = injected.target;
    }

    // 3. Update bindings and trigger events
    for (auto& binding : g_bindings) {
        binding.previousValue = binding.currentValue;
        
        // Only evaluate if context matches one in the stack
        bool contextActive = false;
        for (const auto& ctx : g_contextStack) {
            if (ctx == binding.context) {
                contextActive = true;
                break;
            }
        }
        
        if (contextActive) {
            binding.currentValue = evaluateBinding(binding);
        } else {
            binding.currentValue = 0.0f;
        }
        
    }

    // Events describe the aggregate action, not an individual binding. This
    // prevents a keyboard release from ending an action still held by gamepad.
    for (const auto& sub : g_subscriptions) {
        if (!isContextActive(sub.context)) continue;
        const float current = aggregateActionStrength(sub.actionName, false, &sub.context);
        const float previous = aggregateActionStrength(sub.actionName, true, &sub.context);
        const bool isPressed = current > 0.5f;
        const bool wasPressed = previous > 0.5f;
        if (sub.eventType == InputEvent::Pressed && isPressed && !wasPressed)
            sub.callback(current);
        else if (sub.eventType == InputEvent::Held && isPressed)
            sub.callback(current);
        else if (sub.eventType == InputEvent::Released && !isPressed && wasPressed)
            sub.callback(0.0f);
    }
}

void Input::bindKey(const std::string& action, KeyCode key, const InputContextID& context) {
    ActionBinding b;
    b.actionName = action;
    b.context = context;
    b.isKey = true;
    b.key = key;
    g_bindings.push_back(b);
}

void Input::bindMouse(const std::string& action, MouseButton btn, const InputContextID& context) {
    ActionBinding b;
    b.actionName = action;
    b.context = context;
    b.isMouse = true;
    b.mouseBtn = btn;
    g_bindings.push_back(b);
}

void Input::bindGamepadButton(const std::string& action, GamepadButton button,
                              const InputContextID& context) {
    ActionBinding b;
    b.actionName = action;
    b.context = context;
    b.isGamepadBtn = true;
    b.padBtn = button;
    g_bindings.push_back(b);
}

void Input::bindGamepadAxis(const std::string& action, GamepadAxis axis, float scale, const InputContextID& context) {
    bindGamepadAxis(action, axis, scale,
                    input_detail::kDefaultGamepadDeadzone, context);
}

void Input::bindGamepadAxis(const std::string& action, GamepadAxis axis, float scale,
                            float deadzone, const InputContextID& context) {
    ActionBinding b;
    b.actionName = action;
    b.context = context;
    b.isGamepadAxis = true;
    b.padAxis = axis;
    b.axisScale = scale;
    b.deadzone =
        std::clamp(deadzone, 0.0f, input_detail::kMaxGamepadDeadzone);
    g_bindings.push_back(b);
}

void Input::rebindKey(const std::string& action, KeyCode key,
                      const InputContextID& context) {
    unmapAction(action, context);
    bindKey(action, key, context);
}

void Input::rebindMouse(const std::string& action, MouseButton btn,
                        const InputContextID& context) {
    unmapAction(action, context);
    bindMouse(action, btn, context);
}

void Input::rebindGamepadButton(const std::string& action, GamepadButton button,
                                const InputContextID& context) {
    unmapAction(action, context);
    bindGamepadButton(action, button, context);
}

void Input::rebindGamepadAxis(const std::string& action, GamepadAxis axis,
                              float scale, float deadzone,
                              const InputContextID& context) {
    unmapAction(action, context);
    bindGamepadAxis(action, axis, scale, deadzone, context);
}

void Input::bindTouch(const std::string& action, TouchGesture gesture,
                      glm::vec2 zoneMin, glm::vec2 zoneMax, float minDistance,
                      const InputContextID& context) {
    ActionBinding binding;
    binding.actionName = action;
    binding.context = context;
    binding.isTouch = true;
    binding.touchGesture = gesture;
    const glm::vec2 clampedMin =
        glm::clamp(zoneMin, glm::vec2(0.0f), glm::vec2(1.0f));
    const glm::vec2 clampedMax =
        glm::clamp(zoneMax, glm::vec2(0.0f), glm::vec2(1.0f));
    binding.touchZoneMin = glm::min(clampedMin, clampedMax);
    binding.touchZoneMax = glm::max(clampedMin, clampedMax);
    binding.touchMinDistance = std::clamp(
        minDistance, 0.0f, input_detail::kMaxTouchGestureDistancePixels);
    g_bindings.push_back(std::move(binding));
}

void Input::rebindTouch(const std::string& action, TouchGesture gesture,
                        glm::vec2 zoneMin, glm::vec2 zoneMax, float minDistance,
                        const InputContextID& context) {
    unmapAction(action, context);
    bindTouch(action, gesture, zoneMin, zoneMax, minDistance, context);
}

std::string Input::serializeBindingProfile(const std::string& name) {
    InputBindingProfile profile;
    profile.name = name.empty() || name.size() > 64 ? "default" : name;
    profile.bindings = g_bindings;
    return serializeInputBindingProfile(profile).dump();
}

bool Input::applyBindingProfile(const std::string& serialized,
                                std::string& error) {
    const nlohmann::json doc =
        nlohmann::json::parse(serialized, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) {
        error = "input profile is not valid JSON";
        return false;
    }
    InputBindingProfileParseResult parsed = parseInputBindingProfile(doc);
    if (!parsed.ok) {
        error = std::move(parsed.error);
        return false;
    }
    for (ActionBinding& binding : parsed.profile.bindings) {
        binding.currentValue = 0.0f;
        binding.previousValue = 0.0f;
    }
    g_bindings = std::move(parsed.profile.bindings);
    error.clear();
    return true;
}

void Input::unmapAction(const std::string& action, const InputContextID& context) {
    g_bindings.erase(std::remove_if(g_bindings.begin(), g_bindings.end(), [&](const ActionBinding& b) {
        return b.actionName == action && b.context == context;
    }), g_bindings.end());
}

void Input::clearAllActions() {
    g_bindings.clear();
}

float Input::getActionStrength(const std::string& action) {
    return aggregateActionStrength(action, false);
}

bool Input::isActionHeld(const std::string& action) {
    return getActionStrength(action) > 0.5f;
}

void Input::injectAction(const std::string& action, float strength) {
    g_injectedActions[action].target = strength;
}

void Input::clearInjectedActions() {
    g_injectedActions.clear();
}

void Input::injectDeviceActivity(InputDevice device) {
    g_lastActiveDevice = device;
}

bool Input::isActionJustPressed(const std::string& action) {
    return aggregateActionStrength(action, false) > 0.5f &&
           aggregateActionStrength(action, true) <= 0.5f;
}

bool Input::isActionJustReleased(const std::string& action) {
    return aggregateActionStrength(action, false) <= 0.5f &&
           aggregateActionStrength(action, true) > 0.5f;
}

float Input::getAxis(const std::string& negativeAction, const std::string& positiveAction) {
    return getActionStrength(positiveAction) - getActionStrength(negativeAction);
}

glm::vec2 Input::getVector(const std::string& left, const std::string& right, const std::string& down, const std::string& up) {
    return glm::vec2(getAxis(left, right), getAxis(down, up));
}

bool Input::isKeyDown(KeyCode key) {
    if (!g_window && !g_rawWindow) return false;
    int glfwKey = glfwKeyFromKeyCode(key);
    if (glfwKey >= 0 && glfwKey <= GLFW_KEY_LAST) {
        return g_keyCurr[glfwKey];
    }
    return false;
}

bool Input::isKeyPressed(KeyCode key) {
    if (!g_window && !g_rawWindow) return false;
    int glfwKey = glfwKeyFromKeyCode(key);
    if (glfwKey >= 0 && glfwKey <= GLFW_KEY_LAST) {
        return g_keyCurr[glfwKey] && !g_keyPrev[glfwKey];
    }
    return false;
}

bool Input::isKeyReleased(KeyCode key) {
    if (!g_window && !g_rawWindow) return false;
    int glfwKey = glfwKeyFromKeyCode(key);
    if (glfwKey >= 0 && glfwKey <= GLFW_KEY_LAST) {
        return !g_keyCurr[glfwKey] && g_keyPrev[glfwKey];
    }
    return false;
}

bool Input::isMouseButtonDown(MouseButton btn) {
    if (!g_window && !g_rawWindow) return false;
    int glfwBtn = glfwMouseFromMouseButton(btn);
    if (glfwBtn >= 0 && glfwBtn <= GLFW_MOUSE_BUTTON_LAST) {
        return g_mouseCurr[glfwBtn];
    }
    return false;
}

bool Input::isMouseButtonPressed(MouseButton btn) {
    if (!g_window && !g_rawWindow) return false;
    int glfwBtn = glfwMouseFromMouseButton(btn);
    if (glfwBtn >= 0 && glfwBtn <= GLFW_MOUSE_BUTTON_LAST) {
        return g_mouseCurr[glfwBtn] && !g_mousePrev[glfwBtn];
    }
    return false;
}

bool Input::isMouseButtonReleased(MouseButton btn) {
    if (!g_window && !g_rawWindow) return false;
    int glfwBtn = glfwMouseFromMouseButton(btn);
    if (glfwBtn >= 0 && glfwBtn <= GLFW_MOUSE_BUTTON_LAST) {
        return !g_mouseCurr[glfwBtn] && g_mousePrev[glfwBtn];
    }
    return false;
}

bool Input::isGamepadConnected() { return g_activeGamepad >= 0; }
int Input::activeGamepadId() { return g_activeGamepad; }
const std::string& Input::activeGamepadName() { return g_activeGamepadName; }
bool Input::gamepadBackendAvailable() {
#ifdef __EMSCRIPTEN__
    return emscripten_sample_gamepad_data() == EMSCRIPTEN_RESULT_SUCCESS;
#else
    return true;
#endif
}
bool Input::touchBackendAvailable() {
#ifdef __EMSCRIPTEN__
    return g_touchBackendAvailable;
#else
    return false;
#endif
}
InputDevice Input::lastActiveDevice() { return g_lastActiveDevice; }
const char* Input::deviceName(InputDevice device) {
    switch (device) {
        case InputDevice::None: return "none";
        case InputDevice::KeyboardMouse: return "keyboard-mouse";
        case InputDevice::Gamepad: return "gamepad";
        case InputDevice::Touch: return "touch";
    }
    return "none";
}
bool Input::rumble(float lowFrequency, float highFrequency,
                   uint32_t durationMs) {
    if (g_activeGamepad < 0 ||
        !platform::has(platform::Capability::GamepadInput)) {
        return false;
    }
    lowFrequency = std::clamp(lowFrequency, 0.0f, 1.0f);
    highFrequency = std::clamp(highFrequency, 0.0f, 1.0f);
    durationMs = std::min(durationMs, uint32_t{5000});
    if (durationMs == 0) return stopRumble();
#ifdef __EMSCRIPTEN__
    return saida_web_gamepad_rumble(
               g_activeGamepad, static_cast<double>(lowFrequency),
               static_cast<double>(highFrequency),
               static_cast<int>(durationMs)) != 0;
#else
    // GLFW 3.x exposes no standard haptics API.
    return false;
#endif
}

bool Input::stopRumble() {
    if (g_activeGamepad < 0 ||
        !platform::has(platform::Capability::GamepadInput)) {
        return false;
    }
#ifdef __EMSCRIPTEN__
    return saida_web_gamepad_stop_rumble(g_activeGamepad) != 0;
#else
    return false;
#endif
}

glm::vec2 Input::mouseDelta() { return g_mouseDelta; }
glm::vec2 Input::mousePosition() { return g_mousePos; }
glm::vec2 Input::scrollDelta() { return g_scrollDelta; }
const std::vector<uint32_t>& Input::textInputCodepoints() { return g_textInput; }
const std::vector<TouchPoint>& Input::touches() { return g_touches; }
const std::vector<TouchGestureEvent>& Input::touchGestures() {
    return g_touchGestures;
}

void Input::submitTouch(uint64_t id, glm::vec2 position, TouchPhase phase) {
    glm::vec2 previous = position;
    if (auto it = g_lastTouchPositions.find(id); it != g_lastTouchPositions.end()) {
        previous = it->second;
    }
    g_pendingTouches.push_back({id, position, previous, phase});
    if (phase == TouchPhase::Began) {
        g_touchStartPositions[id] = position;
        g_activeTouchPositions[id] = position;
    } else if (phase == TouchPhase::Moved) {
        g_activeTouchPositions[id] = position;
    } else if (phase == TouchPhase::Ended) {
        glm::vec2 start = previous;
        if (auto it = g_touchStartPositions.find(id);
            it != g_touchStartPositions.end()) {
            start = it->second;
        }
        const glm::vec2 delta = position - start;
        g_pendingTouchGestures.push_back({
            id, input_detail::classifyTouchGesture(start, position),
            start, position, glm::length(delta)});
    }
    if (phase == TouchPhase::Ended || phase == TouchPhase::Cancelled) {
        g_lastTouchPositions.erase(id);
        g_touchStartPositions.erase(id);
        g_activeTouchPositions.erase(id);
    } else {
        g_lastTouchPositions[id] = position;
    }
}

void Input::consumeMouse() {
    for (int b = 0; b <= GLFW_MOUSE_BUTTON_LAST; ++b) {
        g_mouseCurr[b] = false;
        g_mousePrev[b] = false;
    }
    g_mouseDelta = {0.0f, 0.0f};
    g_scrollDelta = {0.0f, 0.0f};
    g_touches.clear();
    g_pendingTouches.clear();
    g_touchGestures.clear();
    g_pendingTouchGestures.clear();
    g_lastTouchPositions.clear();
    g_touchStartPositions.clear();
    g_activeTouchPositions.clear();
    
    // Also clear bindings that are mouse-based
    for (auto& binding : g_bindings) {
        if (binding.isMouse) {
            binding.currentValue = 0.0f;
            binding.previousValue = 0.0f;
        }
    }
}

void Input::consumeKeyboard() {
    for (int k = 0; k <= GLFW_KEY_LAST; ++k) {
        g_keyCurr[k] = false;
        g_keyPrev[k] = false;
    }
    g_textInput.clear();
    
    // Also clear bindings that are key-based
    for (auto& binding : g_bindings) {
        if (binding.isKey) {
            binding.currentValue = 0.0f;
            binding.previousValue = 0.0f;
        }
    }
}

void Input::pushContext(const InputContextID& context) {
    if (std::find(g_contextStack.begin(), g_contextStack.end(), context) == g_contextStack.end()) {
        g_contextStack.push_back(context);
    }
}

void Input::popContext(const InputContextID& context) {
    auto it = std::find(g_contextStack.begin(), g_contextStack.end(), context);
    if (it != g_contextStack.end()) {
        g_contextStack.erase(it);
    }
}

uint32_t Input::subscribe(const std::string& action, InputEvent eventType, ActionCallback callback, const InputContextID& context) {
    Subscription sub;
    sub.id = g_nextSubscriptionId++;
    sub.actionName = action;
    sub.eventType = eventType;
    sub.callback = std::move(callback);
    sub.context = context;
    g_subscriptions.push_back(sub);
    return sub.id;
}

void Input::unsubscribe(uint32_t callbackId) {
    g_subscriptions.erase(std::remove_if(g_subscriptions.begin(), g_subscriptions.end(), [&](const Subscription& s) {
        return s.id == callbackId;
    }), g_subscriptions.end());
}

} // namespace saida
