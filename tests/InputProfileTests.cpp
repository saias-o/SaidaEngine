#include "core/InputProfile.hpp"

#include <cassert>
#include <cmath>

namespace {

saida::ActionBinding keyboard(const char* action, saida::KeyCode key) {
    saida::ActionBinding binding;
    binding.actionName = action;
    binding.isKey = true;
    binding.key = key;
    return binding;
}

saida::ActionBinding axis(const char* action, saida::GamepadAxis control,
                          float scale, float deadzone) {
    saida::ActionBinding binding;
    binding.actionName = action;
    binding.context = "Gameplay";
    binding.isGamepadAxis = true;
    binding.padAxis = control;
    binding.axisScale = scale;
    binding.deadzone = deadzone;
    return binding;
}

saida::ActionBinding touch(const char* action, saida::TouchGesture gesture) {
    saida::ActionBinding binding;
    binding.actionName = action;
    binding.context = "Gameplay";
    binding.isTouch = true;
    binding.touchGesture = gesture;
    binding.touchZoneMin = {0.5f, 0.0f};
    binding.touchZoneMax = {1.0f, 1.0f};
    binding.touchMinDistance = 64.0f;
    return binding;
}

void testRoundTrip() {
    saida::InputBindingProfile source;
    source.name = "southpaw";
    source.bindings.push_back(keyboard("Jump", saida::KeyCode::Space));
    source.bindings.push_back(
        axis("MoveLeft", saida::GamepadAxis::RightX, -1.25f, 0.2f));
    source.bindings.push_back(
        touch("Dash", saida::TouchGesture::SwipeRight));

    const nlohmann::json serialized = saida::serializeInputBindingProfile(source);
    assert(serialized["schema"] == saida::kInputBindingProfileSchema);
    assert(serialized["bindings"][0]["control"] == "Space");
    assert(serialized["bindings"][1]["device"] == "gamepad-axis");
    assert(serialized["bindings"][1]["control"] == "RightX");

    const auto parsed = saida::parseInputBindingProfile(serialized);
    assert(parsed.ok);
    assert(parsed.profile.name == "southpaw");
    assert(parsed.profile.bindings.size() == 3);
    assert(parsed.profile.bindings[0].isKey);
    assert(parsed.profile.bindings[0].key == saida::KeyCode::Space);
    assert(parsed.profile.bindings[1].isGamepadAxis);
    assert(parsed.profile.bindings[1].padAxis == saida::GamepadAxis::RightX);
    assert(std::abs(parsed.profile.bindings[1].axisScale + 1.25f) < 0.0001f);
    assert(std::abs(parsed.profile.bindings[1].deadzone - 0.2f) < 0.0001f);
    assert(parsed.profile.bindings[2].isTouch);
    assert(parsed.profile.bindings[2].touchGesture ==
           saida::TouchGesture::SwipeRight);
    assert(parsed.profile.bindings[2].touchZoneMin.x == 0.5f);
    assert(parsed.profile.bindings[2].touchMinDistance == 64.0f);
}

void testAllControlNamesRoundTrip() {
    for (int i = int(saida::KeyCode::Space); i <= int(saida::KeyCode::Menu); ++i) {
        const auto value = static_cast<saida::KeyCode>(i);
        saida::KeyCode parsed = saida::KeyCode::Unknown;
        assert(saida::parseInputControl(saida::inputControlName(value), parsed));
        assert(parsed == value);
    }
    for (int i = 0; i <= int(saida::MouseButton::Button8); ++i) {
        const auto value = static_cast<saida::MouseButton>(i);
        saida::MouseButton parsed = saida::MouseButton::Left;
        assert(saida::parseInputControl(saida::inputControlName(value), parsed));
        assert(parsed == value);
    }
    for (int i = 0; i <= int(saida::GamepadButton::DpadLeft); ++i) {
        const auto value = static_cast<saida::GamepadButton>(i);
        saida::GamepadButton parsed = saida::GamepadButton::A;
        assert(saida::parseInputControl(saida::inputControlName(value), parsed));
        assert(parsed == value);
    }
    for (int i = 0; i <= int(saida::GamepadAxis::RightTrigger); ++i) {
        const auto value = static_cast<saida::GamepadAxis>(i);
        saida::GamepadAxis parsed = saida::GamepadAxis::LeftX;
        assert(saida::parseInputControl(saida::inputControlName(value), parsed));
        assert(parsed == value);
    }
    for (int i = 0; i <= int(saida::TouchGesture::SwipeDown); ++i) {
        const auto value = static_cast<saida::TouchGesture>(i);
        saida::TouchGesture parsed = saida::TouchGesture::Press;
        assert(saida::parseInputControl(saida::inputControlName(value), parsed));
        assert(parsed == value);
    }
}

void testHostileProfilesFail() {
    auto future = nlohmann::json{
        {"schema", 99}, {"name", "future"}, {"bindings", nlohmann::json::array()}};
    assert(!saida::parseInputBindingProfile(future).ok);

    auto unknownControl = nlohmann::json{
        {"schema", 1},
        {"name", "bad"},
        {"bindings", nlohmann::json::array({
             {{"action", "Jump"}, {"context", "Global"},
              {"device", "keyboard"}, {"control", "LaunchMissiles"}},
         })},
    };
    assert(!saida::parseInputBindingProfile(unknownControl).ok);

    auto invalidAxis = nlohmann::json{
        {"schema", 1},
        {"name", "bad-axis"},
        {"bindings", nlohmann::json::array({
             {{"action", "Move"}, {"context", "Global"},
              {"device", "gamepad-axis"}, {"control", "LeftX"},
              {"deadzone", 1.0}},
         })},
    };
    assert(!saida::parseInputBindingProfile(invalidAxis).ok);

    auto invalidTouch = nlohmann::json{
        {"schema", 1},
        {"name", "bad-touch"},
        {"bindings", nlohmann::json::array({
             {{"action", "Dash"}, {"context", "Global"},
              {"device", "touch"}, {"control", "SwipeRight"},
              {"zone", nlohmann::json::array({0.8, 0.0, 0.2, 1.0})}},
         })},
    };
    assert(!saida::parseInputBindingProfile(invalidTouch).ok);
}

} // namespace

int main() {
    testRoundTrip();
    testAllControlNamesRoundTrip();
    testHostileProfilesFail();
    return 0;
}
