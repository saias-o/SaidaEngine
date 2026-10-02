#include "core/Camera.hpp"
#include "rhi/PipelineState.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

// The scene's depth is reversed (rhi/PipelineState.hpp): near 1, far 0. These
// checks hold the projection to that convention, and prove what it is for --
// that a float depth buffer still tells two surfaces apart a hundred
// kilometres away, which the conventional projection could not.

using namespace saida;

namespace {

int gChecks = 0;

void require(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        std::cerr << "[reversed-depth] FAIL: " << what << "\n";
        std::abort();
    }
}

// The depth a point straight ahead at `distance` metres is stored as.
float depthAt(const Camera& camera, float distance) {
    const glm::vec4 clip = camera.projection() * glm::vec4(0.0f, 0.0f, -distance, 1.0f);
    return clip.z / clip.w;
}

// The conventional (near 0, far 1) depth of the same point, for comparison.
float conventionalDepthAt(float nearZ, float farZ, float distance) {
    const float a = -farZ / (farZ - nearZ), b = -(farZ * nearZ) / (farZ - nearZ);
    return (a * -distance + b) / distance;
}

void testNearIsOneAndFarIsZero() {
    Camera camera;
    camera.setPerspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 200000.0f);
    require(std::abs(depthAt(camera, 0.1f) - 1.0f) < 1e-6f, "the near plane is stored as 1");
    require(std::abs(depthAt(camera, 200000.0f) - rhi::kDepthFar) < 1e-6f, "the far plane is stored as 0");
    require(depthAt(camera, 10.0f) > depthAt(camera, 100.0f), "nearer is greater");
    require(rhi::kDepthCloser == rhi::CompareOp::Greater, "the depth test keeps the greater depth");
}

void testDistantRidgesStayApart() {
    const float nearZ = 0.1f, farZ = 250000.0f;
    Camera camera;
    camera.setPerspective(glm::radians(60.0f), 16.0f / 9.0f, nearZ, farZ);
    // Two ridges a metre apart, 100 km out: stored as two different floats.
    require(depthAt(camera, 100000.0f) != depthAt(camera, 100001.0f),
            "a metre at 100 km is resolved");
    require(depthAt(camera, 100000.0f) > depthAt(camera, 100001.0f), "and in the right order");
    // Conventional depth rounds a whole kilometre there to the same float:
    // the precision this convention exists to recover.
    require(conventionalDepthAt(nearZ, farZ, 100000.0f) == conventionalDepthAt(nearZ, farZ, 101000.0f),
            "conventional depth loses a kilometre at 100 km");
}

void testFrustumStillHoldsTheView() {
    Camera camera;
    camera.position = glm::vec3(0.0f);
    camera.lookAt(glm::vec3(0.0f, 0.0f, -1.0f));
    camera.setPerspective(glm::radians(60.0f), 1.0f, 0.1f, 1000.0f);
    const Frustum f = camera.getFrustum();
    auto inside = [&](glm::vec3 p) {
        for (const glm::vec4& plane : f.planes)
            if (glm::dot(glm::vec3(plane), p) + plane.w < 0.0f) return false;
        return true;
    };
    require(inside({0.0f, 0.0f, -10.0f}), "a point ahead is inside");
    require(inside({0.0f, 0.0f, -999.0f}), "a point before the far plane is inside");
    require(!inside({0.0f, 0.0f, -1001.0f}), "a point past the far plane is outside");
    require(!inside({0.0f, 0.0f, -0.05f}), "a point before the near plane is outside");
    require(!inside({0.0f, 0.0f, 10.0f}), "a point behind is outside");
}

}  // namespace

int main() {
    testNearIsOneAndFarIsZero();
    testDistantRidgesStayApart();
    testFrustumStillHoldsTheView();
    std::cout << "[reversed-depth] OK (" << gChecks << " checks)\n";
    return 0;
}
