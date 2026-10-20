#include "xr/XrMath.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

// The rig (XROrigin) places the tracking space in the world: a head tracked at
// h must be drawn at R(yaw) * h + rig. OpenXR locates poses in the app space,
// through the inverse of the pose that space was created with, so the app
// space must be the inverse of the rig. Built the other way round, walking the
// rig forward moves the world forward with it -- invisible without a headset.

using namespace saida::xr;

namespace {

int gChecks = 0;

void require(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        std::cerr << "[xr-rig] FAIL: " << what << "\n";
        std::abort();
    }
}

bool near(const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b) < 1e-4f; }

// Where OpenXR reports a reference-space point once located in the app space.
glm::vec3 locate(const XrPosef& appSpace, const glm::vec3& tracked) {
    return glm::vec3(glm::inverse(modelFromPose(appSpace)) * glm::vec4(tracked, 1.0f));
}

void testRigTranslatesTheTrackedHead() {
    const glm::vec3 head(0.2f, 1.7f, -0.3f);
    const XrPosef space = appSpacePoseForRig({10.0f, 35.0f, -4.0f}, 0.0f);
    require(near(locate(space, head), {10.2f, 36.7f, -4.3f}), "head = rig + tracked");
}

void testRigYawTurnsAboutTheRig() {
    const float yaw = glm::radians(90.0f);
    const XrPosef space = appSpacePoseForRig({5.0f, 0.0f, 0.0f}, yaw);
    // Looking down -Z, a quarter turn to the left (+Y) faces -X.
    const glm::vec3 ahead = locate(space, {0.0f, 1.6f, -1.0f});
    require(near(ahead, {4.0f, 1.6f, 0.0f}), "+90 deg yaw turns -Z to -X around the rig");
    const glm::quat facing = glm::inverse(toGlm(space.orientation));
    require(near(facing * glm::vec3(0, 0, -1), {-1.0f, 0.0f, 0.0f}), "orientation follows the yaw");
}

void testIdentityRigIsTheTrackingSpace() {
    const XrPosef space = appSpacePoseForRig(glm::vec3(0.0f), 0.0f);
    require(near(locate(space, {1, 2, 3}), {1, 2, 3}), "no rig leaves poses untouched");
}

} // namespace

int main() {
    testRigTranslatesTheTrackedHead();
    testRigYawTurnsAboutTheRig();
    testIdentityRigIsTheTrackingSpace();
    std::cout << "[xr-rig] " << gChecks << " checks passed\n";
    return 0;
}
