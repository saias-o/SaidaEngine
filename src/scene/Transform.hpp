#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace saida {

struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};

    glm::mat4 matrix() const;
};

} // namespace saida
