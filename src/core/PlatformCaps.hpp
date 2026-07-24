#pragma once

// Each player declares at boot what its platform and build actually
// support; a missing system must signal clearly, never fail silently or
// return a misleading neutral value.
//
// The entry point (desktop main, web player...) calls setCapabilities() once;
// the default is "everything" (full desktop) so existing executables are
// unaffected.

#include <cstdint>
#include <string>

namespace saida::platform {

enum class Capability : uint32_t {
    Rendering      = 1u << 0,
    Physics        = 1u << 1,
    Audio          = 1u << 2,
    ScriptGameplay = 1u << 3,  // JavaScript behaviours (QuickJS)
    GameUI         = 1u << 4,  // WebCanvas / RmlUi
    KeyboardMouse  = 1u << 5,
    GamepadInput   = 1u << 6,
    TouchInput     = 1u << 7,
    UserStorage    = 1u << 8,  // player saves / preferences
    XR             = 1u << 9,
};

constexpr uint32_t kAllCapabilities = 0x3FFu;

void setCapabilities(uint32_t mask);
bool has(Capability cap);
const char* name(Capability cap);

// One-line-per-capability report, logged at player boot.
std::string report();

// Gate for optional systems: returns has(cap) and, if absent, logs ONE
// explicit "<feature> requires <cap>, unavailable on this platform" error
// (deduplicated per capability so it doesn't flood the game loop).
bool require(Capability cap, const char* feature);

} // namespace saida::platform
