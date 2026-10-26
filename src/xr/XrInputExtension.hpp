#pragma once

#include "xr/toolkit/XRTypes.hpp"
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace saida::xr {

enum class InputType { Boolean, Float, Vector2, Pose };

struct InputBinding {
    std::string profile;
    std::string userPath;
    std::string componentPath;
};

struct InputAction {
    std::string name;
    InputType type = InputType::Boolean;
    std::vector<InputBinding> bindings;
};

struct InputExtensionDescription {
    std::string name;
    // Atomic all-of dependency set; none are enabled if one is unavailable.
    std::vector<std::string> requiredExtensions;
    std::vector<InputAction> actions;
};

struct InputValue {
    size_t action = 0;
    std::string_view userPath;
    bool active = false;
    bool changed = false;
    bool boolean = false;
    float scalar = 0;
    glm::vec2 vector{0};
    XRPose pose;
};

struct InputExtensionFrame {
    bool available = false; // dependencies and at least one profile accepted
    bool focused = false;   // session focused AND xrSyncActions succeeded
    double time = 0;        // OpenXR predicted display time, seconds
    const InputValue* values = nullptr;
    size_t count = 0;
    const InputValue* begin() const { return values; }
    const InputValue* end() const { return count ? values + count : values; }
};

// Installed before instance creation. Description is copied once at startup;
// snapshots are borrowed only during submit(). The callback must not throw,
// block, allocate per frame, retain views or reenter the engine. Each instance
// belongs to one Engine. No vendor SDK, gesture semantics or game state here.
class InputExtension {
public:
    virtual ~InputExtension() = default;
    virtual InputExtensionDescription describe() const = 0;
    virtual void submit(const InputExtensionFrame& frame) noexcept = 0;
};

using InputExtensionList = std::vector<std::shared_ptr<InputExtension>>;

} // namespace saida::xr
