#pragma once

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

namespace saida {

inline nlohmann::json vec3ToJson(const glm::vec3& v) {
    return nlohmann::json::array({v.x, v.y, v.z});
}

inline glm::vec3 jsonToVec3(const nlohmann::json& j, const glm::vec3& fallback = glm::vec3(0.0f)) {
    if (!j.is_array() || j.size() != 3) return fallback;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

inline glm::vec4 jsonToVec4(const nlohmann::json& j, const glm::vec4& fallback = glm::vec4(0.0f)) {
    if (!j.is_array() || j.size() != 4) return fallback;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>()};
}

} // namespace saida
