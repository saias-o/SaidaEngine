#include "render/TonemapPass.hpp"
#include "scene/Scene.hpp"
#include "xr/XrMath.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace tonemap_shader {
using namespace glm;
#include "depth_reconstruction.glsl"
}  // namespace tonemap_shader

using namespace saida;

namespace {
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "[xr-projection] FAIL: " << message << '\n';
        std::abort();
    }
}

// The shader's own reconstruction (shaders/depth_reconstruction.glsl) of the
// packed inverse projection. The oracle is a world-space surface projected
// through an independently supplied eye pose.
glm::vec3 reconstruct(const TonemapPass::PushConstants& push, glm::vec2 uv, float depth) {
    return tonemap_shader::viewPositionFromDepth(uv, depth, push.projectionParams,
                                                 push.projectionParams2, push.projectionOffsets);
}

const std::array<XrFovf, 2> eyeFovs{{
    {-0.96f, 0.72f, 0.85f, -0.74f},
    {-0.72f, 0.96f, 0.82f, -0.77f}
}};

void testEyeFrustumEdgesAndDepth() {
    constexpr float nearZ = 0.03f, farZ = 30.0f, distance = 0.8f;
    for (const auto& fov : eyeFovs) {
        const auto projection = xr::projectionFromFov(fov, nearZ, farZ);
        auto ndc = [&](glm::vec3 point) {
            const glm::vec4 clip = projection * glm::vec4(point, 1);
            return glm::vec3(clip) / clip.w;
        };
        require(std::abs(ndc({std::tan(fov.angleLeft) * distance, 0, -distance}).x + 1) < 1e-5f,
                "left FOV edge reaches left image edge");
        require(std::abs(ndc({std::tan(fov.angleRight) * distance, 0, -distance}).x - 1) < 1e-5f,
                "right FOV edge reaches right image edge");
        require(std::abs(ndc({0, std::tan(fov.angleUp) * distance, -distance}).y + 1) < 1e-5f,
                "upper FOV edge reaches Vulkan image top");
        require(std::abs(ndc({0, std::tan(fov.angleDown) * distance, -distance}).y - 1) < 1e-5f,
                "lower FOV edge reaches Vulkan image bottom");
        require(std::abs(ndc({0, 0, -nearZ}).z - 1) < 1e-5f, "XR near depth is one");
        require(std::abs(ndc({0, 0, -farZ}).z) < 1e-5f, "XR far depth is zero");
    }
}

void testBothEyesReconstructTheSameMovingWorldSurface() {
    const std::array<glm::vec3, 3> surface{{{-0.15f, 0.85f, -0.6f},
                                         {0.15f, 0.85f, -0.6f},
                                         {-0.15f, 0.85f, -0.9f}}};
    for (float yaw : {-0.4f, 0.0f, 0.4f}) {
        const glm::quat headRotation = glm::angleAxis(yaw, glm::vec3(0, 1, 0))
            * glm::angleAxis(0.12f, glm::vec3(0, 0, 1));
        for (size_t eye = 0; eye < eyeFovs.size(); ++eye) {
            const glm::vec3 position = glm::vec3(0.1f * yaw, 1.6f, 0.05f)
                + headRotation * glm::vec3(eye == 0 ? -0.032f : 0.032f, 0, 0);
            XrPosef pose{{headRotation.x, headRotation.y, headRotation.z, headRotation.w},
                        {position.x, position.y, position.z}};
            const glm::mat4 view = xr::viewFromPose(pose);
            const glm::mat4 projection = xr::projectionFromFov(eyeFovs[eye], 0.03f, 30.0f);
            const auto push = TonemapPass::pushConstants(SceneSettings{}, view, projection, 1);
            require(glm::length(glm::vec2(push.projectionOffsets)) > 0.01f,
                    "test exercises a genuinely off-centre projection");
            std::array<glm::vec3, 3> reconstructed;
            for (size_t i = 0; i < surface.size(); ++i) {
                const glm::vec4 clip = projection * view * glm::vec4(surface[i], 1);
                const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                const glm::vec3 local = reconstruct(push, glm::vec2(ndc) * 0.5f + 0.5f, ndc.z);
                reconstructed[i] = glm::vec3(glm::inverse(view) * glm::vec4(local, 1));
                require(glm::distance(reconstructed[i], surface[i]) < 2e-5f,
                        "AO/fog reconstruct the same surface for both eyes while the head moves");
            }
            const auto normal = glm::normalize(glm::cross(reconstructed[1] - reconstructed[0],
                                                          reconstructed[2] - reconstructed[0]));
            require(glm::distance(normal, glm::vec3(0, 1, 0)) < 1e-4f,
                    "the depth-derived world normal remains stable across eyes and head poses");
            const glm::vec3 shaderEye = -glm::transpose(glm::mat3(view)) * glm::vec3(view[3]);
            require(glm::distance(shaderEye, position) < 1e-5f,
                    "multiview lighting reconstructs the actual eye, including head roll");
        }
    }
}

void testDesktopAndOrthographicReconstruction() {
    auto perspective = glm::perspectiveRH_ZO(glm::radians(65.0f), 1.4f, 30.0f, 0.03f);
    perspective[1][1] *= -1;
    const auto ortho = glm::orthoRH_ZO(-2.0f, 4.0f, -1.0f, 3.0f, 30.0f, 0.03f);
    for (const auto& projection : {perspective, ortho}) {
        const auto push = TonemapPass::pushConstants(SceneSettings{}, glm::mat4(1), projection, 1);
        for (const glm::vec3 point : {glm::vec3(-0.2f, 0.3f, -0.5f), glm::vec3(1, -0.5f, -15)}) {
            const auto clip = projection * glm::vec4(point, 1);
            const auto ndc = glm::vec3(clip) / clip.w;
            require(glm::distance(reconstruct(push, glm::vec2(ndc) * 0.5f + 0.5f, ndc.z), point) < 1e-4f,
                    "desktop perspective and off-centre orthographic projections still round-trip");
        }
    }
}
}

int main() {
    testEyeFrustumEdgesAndDepth();
    testBothEyesReconstructTheSameMovingWorldSurface();
    testDesktopAndOrthographicReconstruction();
    std::cout << "[xr-projection] " << checks << " checks passed without a window\n";
}
