#include "scene/Transform.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace saida {

glm::mat4 Transform::matrix() const {
    // TRS is affine: scale the rotation columns and write the translation,
    // without multiplying two general 4x4 matrices for every visited node.
    glm::mat4 result = glm::mat4_cast(rotation);
    result[0] *= scale.x;
    result[1] *= scale.y;
    result[2] *= scale.z;
    result[3] = glm::vec4(position, 1.f);
    return result;
}

} // namespace saida
