#pragma once

#include "core/InputEnums.hpp"
#include "core/InputLimits.hpp"

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

struct GLFWwindow;

namespace saida {

class Window;

using InputContextID = std::string;
const InputContextID kGlobalContext = "Global";

enum class TouchGesture {
    Press,
    Tap,
    SwipeLeft,
    SwipeRight,
    SwipeUp,
    SwipeDown,
};

struct ActionBinding {
    std::string actionName;
    InputContextID context = kGlobalContext;

    bool isKey = false;
    KeyCode key = KeyCode::Unknown;

    bool isMouse = false;
    MouseButton mouseBtn = MouseButton::Left;

    bool isGamepadBtn = false;
    GamepadButton padBtn = GamepadButton::A;

    bool isGamepadAxis = false;
    GamepadAxis padAxis = GamepadAxis::LeftX;
    float axisScale = 1.0f; // Multiplier (useful for inversion)
    float deadzone = input_detail::kDefaultGamepadDeadzone;

    bool isTouch = false;
    TouchGesture touchGesture = TouchGesture::Press;
    // Normalized zone within the canvas [0, 1], independent of DPI/resolution.
    glm::vec2 touchZoneMin{0.0f};
    glm::vec2 touchZoneMax{1.0f};
    float touchMinDistance = 48.0f; // logical pixels, swipes only

    // Runtime state (updated every frame)
    float currentValue = 0.0f;
    float previousValue = 0.0f;
};

using ActionCallback = std::function<void(float strength)>;

enum class TouchPhase {
    Began,
    Moved,
    Ended,
    Cancelled
};

struct TouchPoint {
    uint64_t id = 0;
    glm::vec2 position{0.0f};
    glm::vec2 previousPosition{0.0f};
    TouchPhase phase = TouchPhase::Moved;
};

struct TouchGestureEvent {
    uint64_t id = 0;
    TouchGesture gesture = TouchGesture::Tap;
    glm::vec2 startPosition{0.0f};
    glm::vec2 endPosition{0.0f};
    float distance = 0.0f;
};

enum class InputDevice {
    None,
    KeyboardMouse,
    Gamepad,
    Touch,
};

class Input {
public:
    // ---- Binding API ----
    static void bindKey(const std::string& action, KeyCode key, const InputContextID& context = kGlobalContext);
    static void bindMouse(const std::string& action, MouseButton btn, const InputContextID& context = kGlobalContext);
    static void bindGamepadButton(const std::string& action, GamepadButton button, const InputContextID& context = kGlobalContext);
    static void bindGamepadAxis(const std::string& action, GamepadAxis axis, float scale = 1.0f, const InputContextID& context = kGlobalContext);
    static void bindGamepadAxis(const std::string& action, GamepadAxis axis, float scale,
                                float deadzone, const InputContextID& context = kGlobalContext);

    // Atomically replaces all of the action's bindings in this context with a
    // single control. These calls form the runtime rebinding surface; a profile
    // can then be exported and persisted by the game.
    static void rebindKey(const std::string& action, KeyCode key,
                          const InputContextID& context = kGlobalContext);
    static void rebindMouse(const std::string& action, MouseButton btn,
                            const InputContextID& context = kGlobalContext);
    static void rebindGamepadButton(const std::string& action, GamepadButton button,
                                    const InputContextID& context = kGlobalContext);
    static void rebindGamepadAxis(const std::string& action, GamepadAxis axis,
                                  float scale = 1.0f,
                                  float deadzone = input_detail::kDefaultGamepadDeadzone,
                                  const InputContextID& context = kGlobalContext);
    static void bindTouch(const std::string& action, TouchGesture gesture,
                          glm::vec2 zoneMin = {0.0f, 0.0f},
                          glm::vec2 zoneMax = {1.0f, 1.0f},
                          float minDistance = 48.0f,
                          const InputContextID& context = kGlobalContext);
    static void rebindTouch(const std::string& action, TouchGesture gesture,
                            glm::vec2 zoneMin = {0.0f, 0.0f},
                            glm::vec2 zoneMax = {1.0f, 1.0f},
                            float minDistance = 48.0f,
                            const InputContextID& context = kGlobalContext);

    // JSON schema 1, no frame state. The application is all-or-nothing:
    // an invalid document leaves current bindings unchanged.
    static std::string serializeBindingProfile(
        const std::string& name = "default");
    static bool applyBindingProfile(const std::string& serialized,
                                    std::string& error);
    
    static void unmapAction(const std::string& action, const InputContextID& context = kGlobalContext);
    static void clearAllActions();

    // ---- Query API (Gameplay) ----
    static bool isActionHeld(const std::string& action);
    static bool isActionJustPressed(const std::string& action);
    static bool isActionJustReleased(const std::string& action);
    static float getActionStrength(const std::string& action);

    // ---- Injection (tests/CI) ----
    // Virtual action strength, combined with real bindings via max. Persists
    // until the next injectAction/clearInjectedActions.
    static void injectAction(const std::string& action, float strength);
    static void clearInjectedActions();
    // Simulates device activity for adaptive-prompt proofs: sets
    // lastActiveDevice as a real transition would. Later real activity
    // naturally takes over again.
    static void injectDeviceActivity(InputDevice device);

    static float getAxis(const std::string& negativeAction, const std::string& positiveAction);
    static glm::vec2 getVector(const std::string& left, const std::string& right, const std::string& down, const std::string& up);

    // ---- Raw Input Queries (Ignores Contexts & UI capture) ----
    static bool isKeyDown(KeyCode key);
    static bool isKeyPressed(KeyCode key);
    static bool isKeyReleased(KeyCode key);
    static bool isMouseButtonDown(MouseButton btn);
    static bool isMouseButtonPressed(MouseButton btn);
    static bool isMouseButtonReleased(MouseButton btn);
    static bool isGamepadConnected();
    static int activeGamepadId();
    static const std::string& activeGamepadName();
    // Backend present in this build and usable on the current platform.
    // On Web, checks navigator.getGamepads via Emscripten without requiring a
    // gamepad to already be connected.
    static bool gamepadBackendAvailable();
    static bool touchBackendAvailable();
    static InputDevice lastActiveDevice();
    static const char* deviceName(InputDevice device);
    // Dynamic haptics on the active pad. Returns false when the backend or
    // the dual-rumble actuator is absent; no fake success is simulated.
    static bool rumble(float lowFrequency, float highFrequency,
                       uint32_t durationMs);
    static bool stopRumble();

    static glm::vec2 mouseDelta();          // movement since last frame
    static glm::vec2 mousePosition();       // cursor position in window pixels
    static glm::vec2 scrollDelta();         // wheel/trackpad delta since last frame
    static const std::vector<uint32_t>& textInputCodepoints();
    static const std::vector<TouchPoint>& touches();
    static const std::vector<TouchGestureEvent>& touchGestures();

    // Platform backends with native touch (mobile/web) enqueue events before
    // Engine::run samples the frame. Desktop GLFW leaves this empty.
    static void submitTouch(uint64_t id, glm::vec2 position, TouchPhase phase);

    // ---- Event Consumption ----
    static void consumeMouse();
    static void consumeKeyboard();

    // UI backends publish capture intent without coupling Input to a specific UI.
    static void setUiCapture(bool keyboard, bool mouse);
    static bool uiCapturesKeyboard();
    static bool uiCapturesMouse();

    // ---- Context Management ----
    static void pushContext(const InputContextID& context);
    static void popContext(const InputContextID& context);

    // ---- Callback System ----
    static uint32_t subscribe(const std::string& action, InputEvent eventType, ActionCallback callback, const InputContextID& context = kGlobalContext);
    static void unsubscribe(uint32_t callbackId);

    // Direct attachment to a raw GLFW window — the path for players without
    // the desktop Window wrapper (web/Emscripten). Installs the mouse
    // delta/scroll/text callbacks internally and the same default bindings as
    // bind().
    static void bindRaw(GLFWwindow* window);

    // Samples a frame for players driving their own loop. Engine uses
    // newFrame() directly.
    static void sample() { newFrame(); }

private:
    friend class Engine;
    static void bind(Window* window);  // called once at startup
    static void newFrame();            // sample state; called once per frame

    // Helper to evaluate binding value from raw input
    static float evaluateBinding(const ActionBinding& binding);
};

} // namespace saida
