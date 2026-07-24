#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <cstdint>

namespace saida {

// Compile-time or runtime string hashing (FNV-1a 32-bit)
constexpr uint32_t hashString(std::string_view str) {
    uint32_t hash = 0x811c9dc5;
    for (char c : str) {
        hash ^= static_cast<uint32_t>(c);
        hash *= 0x01000193;
    }
    return hash;
}

// Ultra-fast parameter storage for the animation system.
// Uses integer hashes instead of strings to guarantee high performance on mobile/VR.
// All values (including booleans) are stored as floats.
class AnimBlackboard {
public:
    void setFloat(uint32_t paramHash, float value);
    void setFloat(std::string_view paramName, float value);

    void setBool(uint32_t paramHash, bool value);
    void setBool(std::string_view paramName, bool value);

    float getFloat(uint32_t paramHash, float defaultValue = 0.0f) const;
    float getFloat(std::string_view paramName, float defaultValue = 0.0f) const;

    bool getBool(uint32_t paramHash, bool defaultValue = false) const;
    bool getBool(std::string_view paramName, bool defaultValue = false) const;

    // A trigger is 1 until a transition that consumes it is taken (the
    // state machine then calls resetTrigger).
    void setTrigger(uint32_t paramHash) { setFloat(paramHash, 1.0f); }
    void setTrigger(std::string_view paramName) { setFloat(paramName, 1.0f); }
    void resetTrigger(uint32_t paramHash) { setFloat(paramHash, 0.0f); }

private:
    std::unordered_map<uint32_t, float> parameters_;
};

} // namespace saida
