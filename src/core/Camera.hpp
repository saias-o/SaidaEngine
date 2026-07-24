#pragma once

#include <glm/glm.hpp>

namespace saida {

// A yaw/pitch fly camera. Holds position + orientation and produces Vulkan-ready
// view and projection matrices (the projection already flips Y for Vulkan).

struct Frustum {
    glm::vec4 planes[6];
};

class Camera {
public:
    void setPerspective(float fovYRadians, float aspect, float nearZ, float farZ);

    glm::mat4 view() const;
    glm::mat4 projection() const { return projection_; }
    Frustum getFrustum() const;

    // Direction vectors derived from yaw/pitch (world up is +Y).
    glm::vec3 front() const;
    glm::vec3 right() const;
    glm::vec3 up() const;

    // Rotate by mouse deltas (degrees); pitch is clamped to avoid flipping.
    void rotate(float deltaYawDeg, float deltaPitchDeg);

    // Point the camera at a world position (sets yaw/pitch from the direction).
    // Used by the CameraDirector to drive the camera from a blended pose.
    void lookAt(const glm::vec3& target);

    glm::vec3 position{0.0f, 0.0f, 3.0f};
    float yaw = -90.0f;        // degrees; -90 looks toward -Z
    float pitch = 0.0f;        // degrees
    float fovDegrees = 45.0f;  // vertical field of view (driven per-camera)
    float nearZ = 0.1f;        // near clip plane (m)   — driven per-camera
    float farZ = 1000.0f;      // far clip plane (m) / view distance — driven per-camera

private:
    glm::mat4 projection_{1.0f};
};

} // namespace saida
