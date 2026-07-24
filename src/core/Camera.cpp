#include "core/Camera.hpp"

#include <glm/gtc/matrix_transform.hpp>  // GLM_FORCE_* set globally by CMake

#include <algorithm>

namespace saida {

namespace {
constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};
}

void Camera::setPerspective(float fovYRadians, float aspect, float nearZ, float farZ) {
    projection_ = glm::perspective(fovYRadians, aspect, nearZ, farZ);
    projection_[1][1] *= -1.0f;  // GLM is OpenGL-handed; flip Y for Vulkan.
}

glm::vec3 Camera::front() const {
    float y = glm::radians(yaw);
    float p = glm::radians(pitch);
    return glm::normalize(glm::vec3(std::cos(y) * std::cos(p),
                                    std::sin(p),
                                    std::sin(y) * std::cos(p)));
}

glm::vec3 Camera::right() const {
    return glm::normalize(glm::cross(front(), kWorldUp));
}

glm::vec3 Camera::up() const {
    return glm::normalize(glm::cross(right(), front()));
}

void Camera::rotate(float deltaYawDeg, float deltaPitchDeg) {
    yaw += deltaYawDeg;
    pitch = std::clamp(pitch + deltaPitchDeg, -89.0f, 89.0f);
}

void Camera::lookAt(const glm::vec3& target) {
    glm::vec3 dir = target - position;
    if (glm::dot(dir, dir) < 1e-12f) return;  // target == position: keep orientation
    dir = glm::normalize(dir);
    // Inverse of front(): yaw from XZ, pitch from Y (world up is +Y).
    pitch = glm::degrees(std::asin(std::clamp(dir.y, -1.0f, 1.0f)));
    yaw = glm::degrees(std::atan2(dir.z, dir.x));
}

glm::mat4 Camera::view() const {
    return glm::lookAt(position, position + front(), kWorldUp);
}

Frustum Camera::getFrustum() const {
    Frustum f;
    glm::mat4 m = projection_ * view();
    // In GLM, m[col][row]. We need to add rows.
    // row0 = vec4(m[0][0], m[1][0], m[2][0], m[3][0])
    // row1 = vec4(m[0][1], m[1][1], m[2][1], m[3][1])
    // row2 = vec4(m[0][2], m[1][2], m[2][2], m[3][2])
    // row3 = vec4(m[0][3], m[1][3], m[2][3], m[3][3])

    glm::vec4 row0 = glm::vec4(m[0][0], m[1][0], m[2][0], m[3][0]);
    glm::vec4 row1 = glm::vec4(m[0][1], m[1][1], m[2][1], m[3][1]);
    glm::vec4 row2 = glm::vec4(m[0][2], m[1][2], m[2][2], m[3][2]);
    glm::vec4 row3 = glm::vec4(m[0][3], m[1][3], m[2][3], m[3][3]);

    // Left
    f.planes[0] = row3 + row0;
    // Right
    f.planes[1] = row3 - row0;
    // Bottom
    f.planes[2] = row3 + row1;
    // Top
    f.planes[3] = row3 - row1;
    // Near (Vulkan uses depth 0 to 1, so z_c >= 0 -> row2 >= 0)
    f.planes[4] = row2;
    // Far
    f.planes[5] = row3 - row2;

    for (int i = 0; i < 6; ++i) {
        float length = glm::length(glm::vec3(f.planes[i]));
        f.planes[i] /= length;
    }
    return f;
}

} // namespace saida
