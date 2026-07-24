#include "scene/animation/AnimBlackboard.hpp"

namespace saida {

void AnimBlackboard::setFloat(uint32_t paramHash, float value) {
    parameters_[paramHash] = value;
}

void AnimBlackboard::setFloat(std::string_view paramName, float value) {
    setFloat(hashString(paramName), value);
}

void AnimBlackboard::setBool(uint32_t paramHash, bool value) {
    parameters_[paramHash] = value ? 1.0f : 0.0f;
}

void AnimBlackboard::setBool(std::string_view paramName, bool value) {
    setBool(hashString(paramName), value);
}

float AnimBlackboard::getFloat(uint32_t paramHash, float defaultValue) const {
    auto it = parameters_.find(paramHash);
    if (it != parameters_.end()) {
        return it->second;
    }
    return defaultValue;
}

float AnimBlackboard::getFloat(std::string_view paramName, float defaultValue) const {
    return getFloat(hashString(paramName), defaultValue);
}

bool AnimBlackboard::getBool(uint32_t paramHash, bool defaultValue) const {
    return getFloat(paramHash, defaultValue ? 1.0f : 0.0f) > 0.5f;
}

bool AnimBlackboard::getBool(std::string_view paramName, bool defaultValue) const {
    return getBool(hashString(paramName), defaultValue);
}

} // namespace saida
