#pragma once

#include "core/Input.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace saida {

constexpr int kInputBindingProfileSchema = 1;

struct InputBindingProfile {
    std::string name = "default";
    std::vector<ActionBinding> bindings;
};

struct InputBindingProfileParseResult {
    bool ok = false;
    InputBindingProfile profile;
    std::string error;
};

const char* inputControlName(KeyCode value);
const char* inputControlName(MouseButton value);
const char* inputControlName(GamepadButton value);
const char* inputControlName(GamepadAxis value);
const char* inputControlName(TouchGesture value);

bool parseInputControl(std::string_view name, KeyCode& out);
bool parseInputControl(std::string_view name, MouseButton& out);
bool parseInputControl(std::string_view name, GamepadButton& out);
bool parseInputControl(std::string_view name, GamepadAxis& out);
bool parseInputControl(std::string_view name, TouchGesture& out);

nlohmann::json serializeInputBindingProfile(const InputBindingProfile& profile);
InputBindingProfileParseResult parseInputBindingProfile(const nlohmann::json& doc);

} // namespace saida
