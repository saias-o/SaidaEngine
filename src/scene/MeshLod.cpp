#include "scene/MeshLod.hpp"

#include "graphics/Mesh.hpp"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace saida {

namespace {

float defaultCoverageThreshold(size_t lodIndex, size_t lodCount) {
    if (lodCount <= 1) return 0.0f;
    if (lodIndex + 1 >= lodCount) return 0.0f;

    static constexpr float kDefaults[] = {0.50f, 0.25f, 0.08f};
    constexpr size_t kDefaultCount = sizeof(kDefaults) / sizeof(kDefaults[0]);
    if (lodIndex < kDefaultCount) return kDefaults[lodIndex];

    return std::max(0.01f, kDefaults[kDefaultCount - 1] *
        std::pow(0.5f, static_cast<float>(lodIndex + 1 - kDefaultCount)));
}

} // namespace

float computeScreenCoverage(const glm::mat4& world, const Aabb& localBounds,
                            const glm::mat4& view, const glm::mat4& proj) {
    const glm::vec3 worldCenter = glm::vec3(world * glm::vec4(localBounds.center(), 1.0f));
    const float maxScale = std::max({
        glm::length(glm::vec3(world[0])),
        glm::length(glm::vec3(world[1])),
        glm::length(glm::vec3(world[2]))
    });
    const float worldRadius = glm::length(localBounds.extent()) * 0.5f * maxScale;

    const glm::vec4 viewPos = view * glm::vec4(worldCenter, 1.0f);
    const float dist = glm::max(-viewPos.z, 0.001f);

    const float projY = std::abs(proj[1][1]);
    if (worldRadius <= 0.0f || projY <= 0.0001f) return 0.0f;

    const float tanHalfFovY = 1.0f / projY;
    const float ndcRadius = (worldRadius / dist) / tanHalfFovY;
    const float diameter = glm::clamp(2.0f * ndcRadius, 0.0f, 2.0f);
    return glm::min(1.0f, diameter * diameter);
}

int selectLodIndex(float screenCoverage, const std::vector<MeshLodLevel>& lods) {
    if (lods.empty()) return 0;
    for (int i = 0; i < static_cast<int>(lods.size()); ++i) {
        if (screenCoverage >= lods[static_cast<size_t>(i)].minScreenCoverage)
            return i;
    }
    return static_cast<int>(lods.size()) - 1;
}

std::vector<float> coverageThresholdsFromMsft(const std::vector<float>& msftCoverage, size_t lodCount) {
    std::vector<float> thresholds(lodCount, 0.0f);
    for (size_t i = 0; i < lodCount; ++i) {
        if (i + 1 < msftCoverage.size()) {
            thresholds[i] = msftCoverage[i + 1];
        } else {
            thresholds[i] = defaultCoverageThreshold(i, lodCount);
        }
    }
    return thresholds;
}

} // namespace saida
