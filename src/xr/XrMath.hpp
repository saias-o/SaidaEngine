#pragma once

// OpenXR pose/FOV -> GLM matrices in the engine's Vulkan conventions.

#include <openxr/openxr.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace saida::xr {

struct Transform {
    glm::vec3 position;
    glm::quat rotation;
};

inline glm::vec3 toGlm(const XrVector3f& v) { return {v.x, v.y, v.z}; }
inline glm::quat toGlm(const XrQuaternionf& q) { return glm::quat(q.w, q.x, q.y, q.z); }

inline Transform poseToTransform(const XrPosef& pose) {
    return {toGlm(pose.position), toGlm(pose.orientation)};
}

// Model matrix of a pose (rotate then translate), then its inverse = view matrix.
inline glm::mat4 modelFromPose(const XrPosef& pose) {
    glm::mat4 t = glm::translate(glm::mat4(1.0f), toGlm(pose.position));
    glm::mat4 r = glm::mat4_cast(toGlm(pose.orientation));
    return t * r;
}

inline glm::mat4 viewFromPose(const XrPosef& pose) {
    return glm::inverse(modelFromPose(pose));
}

// Eye height a LOCAL reference space puts its origin at: LOCAL is centred on
// the head at startup, STAGE on the floor. The rig (XROrigin) is the floor in
// both, so a LOCAL space is lifted by this much.
inline constexpr float kLocalSpaceEyeHeight = 1.6f;

// The pose to create the app space with (poseInReferenceSpace) so that a
// tracked pose comes back as rig * tracked: world = R(yaw) * tracked + rig.
// OpenXR locates views *in* the app space, i.e. through the inverse of this
// pose, so the app space is the inverse of the rig.
inline XrPosef appSpacePoseForRig(const glm::vec3& rigPosition, float rigYawRadians) {
    const glm::quat inverse = glm::angleAxis(-rigYawRadians, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 position = inverse * -rigPosition;
    XrPosef pose{};
    pose.orientation = {inverse.x, inverse.y, inverse.z, inverse.w};
    pose.position = {position.x, position.y, position.z};
    return pose;
}

// Vulkan Y-down clip space is baked in here; callers must not flip Y again.
// Depth is reversed like the desktop camera's: near 1, far 0.
inline glm::mat4 projectionFromFov(const XrFovf& fov, float nearZ, float farZ) {
    const float tanLeft  = std::tan(fov.angleLeft);
    const float tanRight = std::tan(fov.angleRight);
    const float tanDown  = std::tan(fov.angleDown);
    const float tanUp    = std::tan(fov.angleUp);

    const float tanWidth  = tanRight - tanLeft;
    const float tanHeight = tanDown - tanUp;  // Vulkan: positive Y down.

    glm::mat4 m(0.0f);
    m[0][0] = 2.0f / tanWidth;
    m[1][1] = 2.0f / tanHeight;
    m[2][0] = (tanRight + tanLeft) / tanWidth;
    m[2][1] = (tanUp + tanDown) / tanHeight;
    m[2][2] = nearZ / (farZ - nearZ);
    m[2][3] = -1.0f;
    m[3][2] = (farZ * nearZ) / (farZ - nearZ);
    return m;
}

} // namespace saida::xr
