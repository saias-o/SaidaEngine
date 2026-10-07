#include "render/LensFlare.hpp"

#include "nodes/LightNode.hpp"
#include "scene/Scene.hpp"

#include <algorithm>

namespace saida {

namespace {
// Rec. 709 luminance: which of two lights reads brighter, not how bright.
constexpr glm::vec3 kLuminance{0.2126f, 0.7152f, 0.0722f};
constexpr float kMinDirectionLength = 1e-6f;
} // namespace

LensFlareSource pickLensFlareSource(const Scene& scene) {
    LensFlareSource best;
    float bestLuminance = 0.0f;
    for (const LightNode* light : scene.lights()) {
        if (!light || light->type != LightType::Directional) continue;
        if (!light->lensFlare && !light->sunStar) continue;
        const glm::vec3 radiance = glm::max(light->color, glm::vec3(0.0f)) *
                                   std::max(light->intensity, 0.0f);
        const float luminance = glm::dot(radiance, kLuminance);
        if (!(luminance > bestLuminance)) continue;
        // `direction` is where the light travels; the source lies the other way.
        const glm::vec3 travel = glm::mat3(light->worldTransform()) * light->direction;
        const float length = glm::length(travel);
        if (!(length > kMinDirectionLength)) continue;
        best.towardLight = -travel / length;
        best.radiance = radiance;
        best.lensFlare = light->lensFlare;
        best.sunStar = light->sunStar;
        bestLuminance = luminance;
    }
    return best;
}

} // namespace saida
