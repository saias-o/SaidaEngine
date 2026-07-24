#include "scene/Transform.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace saida {

glm::mat4 Transform::matrix() const {
    glm::mat4 result = glm::translate(glm::mat4(1.0f), position);
    result *= glm::mat4_cast(rotation);
    return glm::scale(result, scale);
}

} // namespace saida
