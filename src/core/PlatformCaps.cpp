#include "core/PlatformCaps.hpp"

#include "core/Log.hpp"

namespace saida::platform {

namespace {
uint32_t gCapabilities = kAllCapabilities;
uint32_t gReported = 0;  // absent capabilities already reported by require()

constexpr Capability kAll[] = {
    Capability::Rendering,     Capability::Physics,     Capability::Audio,
    Capability::ScriptGameplay, Capability::GameUI,     Capability::KeyboardMouse,
    Capability::GamepadInput,  Capability::TouchInput,  Capability::UserStorage,
    Capability::XR,
};
} // namespace

void setCapabilities(uint32_t mask) {
    gCapabilities = mask;
    gReported = 0;
}

bool has(Capability cap) {
    return (gCapabilities & static_cast<uint32_t>(cap)) != 0;
}

const char* name(Capability cap) {
    switch (cap) {
        case Capability::Rendering: return "rendering";
        case Capability::Physics: return "physics";
        case Capability::Audio: return "audio";
        case Capability::ScriptGameplay: return "script-gameplay";
        case Capability::GameUI: return "game-ui";
        case Capability::KeyboardMouse: return "keyboard-mouse";
        case Capability::GamepadInput: return "gamepad";
        case Capability::TouchInput: return "touch";
        case Capability::UserStorage: return "user-storage";
        case Capability::XR: return "xr";
    }
    return "unknown";
}

std::string report() {
    std::string out = "platform capabilities:";
    for (Capability cap : kAll) {
        out += " ";
        out += name(cap);
        out += has(cap) ? "=yes" : "=NO";
    }
    return out;
}

bool require(Capability cap, const char* feature) {
    if (has(cap)) return true;
    const uint32_t bit = static_cast<uint32_t>(cap);
    if ((gReported & bit) == 0) {
        gReported |= bit;
        Log::error("'", feature, "' requires the '", name(cap),
                   "' capability, unavailable on this platform — feature disabled");
    }
    return false;
}

} // namespace saida::platform
