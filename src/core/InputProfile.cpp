#include "core/InputProfile.hpp"

#include <array>
#include <cmath>

namespace saida {

namespace {

constexpr std::array kKeyNames{
    "Unknown",
    "Space", "Apostrophe", "Comma", "Minus", "Period", "Slash",
    "Num0", "Num1", "Num2", "Num3", "Num4", "Num5", "Num6", "Num7", "Num8", "Num9",
    "Semicolon", "Equal",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "LeftBracket", "Backslash", "RightBracket", "GraveAccent",
    "Escape", "Enter", "Tab", "Backspace", "Insert", "Delete",
    "Right", "Left", "Down", "Up", "PageUp", "PageDown", "Home", "End",
    "CapsLock", "ScrollLock", "NumLock", "PrintScreen", "Pause",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24",
    "Keypad0", "Keypad1", "Keypad2", "Keypad3", "Keypad4",
    "Keypad5", "Keypad6", "Keypad7", "Keypad8", "Keypad9",
    "KeypadDecimal", "KeypadDivide", "KeypadMultiply", "KeypadSubtract",
    "KeypadAdd", "KeypadEnter", "KeypadEqual",
    "LeftShift", "LeftControl", "LeftAlt", "LeftSuper",
    "RightShift", "RightControl", "RightAlt", "RightSuper", "Menu",
};

constexpr std::array kMouseNames{
    "Left", "Right", "Middle", "Button4", "Button5", "Button6", "Button7", "Button8",
};

constexpr std::array kGamepadButtonNames{
    "A", "B", "X", "Y", "LeftBumper", "RightBumper", "Back", "Start", "Guide",
    "LeftThumb", "RightThumb", "DpadUp", "DpadRight", "DpadDown", "DpadLeft",
};

constexpr std::array kGamepadAxisNames{
    "LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger",
};

constexpr std::array kTouchGestureNames{
    "Press", "Tap", "SwipeLeft", "SwipeRight", "SwipeUp", "SwipeDown",
};

static_assert(kKeyNames.size() == static_cast<size_t>(KeyCode::Menu) + 1);
static_assert(kMouseNames.size() == static_cast<size_t>(MouseButton::Button8) + 1);
static_assert(kGamepadButtonNames.size() ==
              static_cast<size_t>(GamepadButton::DpadLeft) + 1);
static_assert(kGamepadAxisNames.size() ==
              static_cast<size_t>(GamepadAxis::RightTrigger) + 1);
static_assert(kTouchGestureNames.size() ==
              static_cast<size_t>(TouchGesture::SwipeDown) + 1);

template <typename Enum, size_t N>
const char* enumName(Enum value, const std::array<const char*, N>& names) {
    const size_t index = static_cast<size_t>(value);
    return index < names.size() ? names[index] : "Unknown";
}

template <typename Enum, size_t N>
bool parseEnum(std::string_view name, const std::array<const char*, N>& names,
               Enum& out) {
    for (size_t i = 0; i < names.size(); ++i) {
        if (name == names[i]) {
            out = static_cast<Enum>(i);
            return true;
        }
    }
    return false;
}

bool validIdentifier(const nlohmann::json& value, size_t maxLength) {
    return value.is_string() && !value.get_ref<const std::string&>().empty() &&
           value.get_ref<const std::string&>().size() <= maxLength;
}

InputBindingProfileParseResult profileError(std::string error) {
    InputBindingProfileParseResult result;
    result.error = std::move(error);
    return result;
}

} // namespace

const char* inputControlName(KeyCode value) {
    return enumName(value, kKeyNames);
}

const char* inputControlName(MouseButton value) {
    return enumName(value, kMouseNames);
}

const char* inputControlName(GamepadButton value) {
    return enumName(value, kGamepadButtonNames);
}

const char* inputControlName(GamepadAxis value) {
    return enumName(value, kGamepadAxisNames);
}

const char* inputControlName(TouchGesture value) {
    return enumName(value, kTouchGestureNames);
}

bool parseInputControl(std::string_view name, KeyCode& out) {
    return parseEnum(name, kKeyNames, out) && out != KeyCode::Unknown;
}

bool parseInputControl(std::string_view name, MouseButton& out) {
    return parseEnum(name, kMouseNames, out);
}

bool parseInputControl(std::string_view name, GamepadButton& out) {
    return parseEnum(name, kGamepadButtonNames, out);
}

bool parseInputControl(std::string_view name, GamepadAxis& out) {
    return parseEnum(name, kGamepadAxisNames, out);
}

bool parseInputControl(std::string_view name, TouchGesture& out) {
    return parseEnum(name, kTouchGestureNames, out);
}

nlohmann::json serializeInputBindingProfile(const InputBindingProfile& profile) {
    nlohmann::json bindings = nlohmann::json::array();
    for (const ActionBinding& binding : profile.bindings) {
        nlohmann::json item{
            {"action", binding.actionName},
            {"context", binding.context},
        };
        if (binding.isKey) {
            item["device"] = "keyboard";
            item["control"] = inputControlName(binding.key);
        } else if (binding.isMouse) {
            item["device"] = "mouse";
            item["control"] = inputControlName(binding.mouseBtn);
        } else if (binding.isGamepadBtn) {
            item["device"] = "gamepad-button";
            item["control"] = inputControlName(binding.padBtn);
        } else if (binding.isGamepadAxis) {
            item["device"] = "gamepad-axis";
            item["control"] = inputControlName(binding.padAxis);
            item["scale"] = binding.axisScale;
            item["deadzone"] = binding.deadzone;
        } else if (binding.isTouch) {
            item["device"] = "touch";
            item["control"] = inputControlName(binding.touchGesture);
            item["zone"] = {
                binding.touchZoneMin.x, binding.touchZoneMin.y,
                binding.touchZoneMax.x, binding.touchZoneMax.y,
            };
            item["minDistance"] = binding.touchMinDistance;
        } else {
            item["device"] = "invalid";
        }
        bindings.push_back(std::move(item));
    }
    return {
        {"schema", kInputBindingProfileSchema},
        {"name", profile.name},
        {"bindings", std::move(bindings)},
    };
}

InputBindingProfileParseResult parseInputBindingProfile(const nlohmann::json& doc) {
    if (!doc.is_object()) return profileError("profile must be a JSON object");
    if (!doc.contains("schema") || !doc["schema"].is_number_integer())
        return profileError("profile.schema must be an integer");
    if (doc["schema"].get<int>() != kInputBindingProfileSchema)
        return profileError("unsupported input profile schema");
    if (!doc.contains("name") || !validIdentifier(doc["name"], 64))
        return profileError("profile.name must contain 1..64 characters");
    if (!doc.contains("bindings") || !doc["bindings"].is_array())
        return profileError("profile.bindings must be an array");
    if (doc["bindings"].size() > 2048)
        return profileError("profile.bindings exceeds the 2048 entry limit");

    InputBindingProfileParseResult result;
    result.profile.name = doc["name"].get<std::string>();
    result.profile.bindings.reserve(doc["bindings"].size());

    for (size_t i = 0; i < doc["bindings"].size(); ++i) {
        const nlohmann::json& item = doc["bindings"][i];
        const std::string prefix = "profile.bindings[" + std::to_string(i) + "]";
        if (!item.is_object()) return profileError(prefix + " must be an object");
        if (!item.contains("action") || !validIdentifier(item["action"], 128))
            return profileError(prefix + ".action must contain 1..128 characters");
        if (!item.contains("context") || !validIdentifier(item["context"], 128))
            return profileError(prefix + ".context must contain 1..128 characters");
        if (!item.contains("device") || !item["device"].is_string())
            return profileError(prefix + ".device must be a string");
        if (!item.contains("control") || !item["control"].is_string())
            return profileError(prefix + ".control must be a string");

        ActionBinding binding;
        binding.actionName = item["action"].get<std::string>();
        binding.context = item["context"].get<std::string>();
        const std::string device = item["device"].get<std::string>();
        const std::string control = item["control"].get<std::string>();

        if (device == "keyboard") {
            binding.isKey = true;
            if (!parseInputControl(control, binding.key))
                return profileError(prefix + ".control is not a supported key");
        } else if (device == "mouse") {
            binding.isMouse = true;
            if (!parseInputControl(control, binding.mouseBtn))
                return profileError(prefix + ".control is not a supported mouse button");
        } else if (device == "gamepad-button") {
            binding.isGamepadBtn = true;
            if (!parseInputControl(control, binding.padBtn))
                return profileError(prefix + ".control is not a supported gamepad button");
        } else if (device == "gamepad-axis") {
            binding.isGamepadAxis = true;
            if (!parseInputControl(control, binding.padAxis))
                return profileError(prefix + ".control is not a supported gamepad axis");
            if (item.contains("scale")) {
                if (!item["scale"].is_number())
                    return profileError(prefix + ".scale must be numeric");
                binding.axisScale = item["scale"].get<float>();
            }
            if (item.contains("deadzone")) {
                if (!item["deadzone"].is_number())
                    return profileError(prefix + ".deadzone must be numeric");
                binding.deadzone = item["deadzone"].get<float>();
            }
            if (!std::isfinite(binding.axisScale) || std::abs(binding.axisScale) > 10.0f)
                return profileError(prefix + ".scale must be finite and within [-10, 10]");
            if (!std::isfinite(binding.deadzone) ||
                binding.deadzone < 0.0f ||
                binding.deadzone > input_detail::kMaxGamepadDeadzone)
                return profileError(prefix + ".deadzone must be within [0, 0.99]");
        } else if (device == "touch") {
            binding.isTouch = true;
            if (!parseInputControl(control, binding.touchGesture))
                return profileError(prefix + ".control is not a supported touch gesture");
            if (!item.contains("zone") || !item["zone"].is_array() ||
                item["zone"].size() != 4) {
                return profileError(prefix + ".zone must contain four numbers");
            }
            for (const nlohmann::json& coordinate : item["zone"]) {
                if (!coordinate.is_number())
                    return profileError(prefix + ".zone must contain four numbers");
            }
            binding.touchZoneMin = {
                item["zone"][0].get<float>(), item["zone"][1].get<float>()};
            binding.touchZoneMax = {
                item["zone"][2].get<float>(), item["zone"][3].get<float>()};
            if (!std::isfinite(binding.touchZoneMin.x) ||
                !std::isfinite(binding.touchZoneMin.y) ||
                !std::isfinite(binding.touchZoneMax.x) ||
                !std::isfinite(binding.touchZoneMax.y) ||
                binding.touchZoneMin.x < 0.0f ||
                binding.touchZoneMin.y < 0.0f ||
                binding.touchZoneMax.x > 1.0f ||
                binding.touchZoneMax.y > 1.0f ||
                binding.touchZoneMin.x > binding.touchZoneMax.x ||
                binding.touchZoneMin.y > binding.touchZoneMax.y) {
                return profileError(prefix + ".zone must be ordered within [0, 1]");
            }
            if (item.contains("minDistance")) {
                if (!item["minDistance"].is_number())
                    return profileError(prefix + ".minDistance must be numeric");
                binding.touchMinDistance = item["minDistance"].get<float>();
            }
            if (!std::isfinite(binding.touchMinDistance) ||
                binding.touchMinDistance < 0.0f ||
                binding.touchMinDistance >
                    input_detail::kMaxTouchGestureDistancePixels) {
                return profileError(
                    prefix + ".minDistance must be within [0, 4096]");
            }
        } else {
            return profileError(prefix + ".device is unsupported");
        }

        result.profile.bindings.push_back(std::move(binding));
    }

    result.ok = true;
    return result;
}

} // namespace saida
