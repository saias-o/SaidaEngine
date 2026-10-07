// Locks the CPU half of a directional light's lens effects (LightNode
// lensFlare / sunStar): which light is the source, where its source lands on
// screen in the tonemap's convention, and that nothing is drawn for a scene
// that did not ask, a source behind the camera or one well off screen. The
// pixels themselves are the tonemap shader's and are checked by eye.

#include "core/Reflection.hpp"
#include "nodes/LightNode.hpp"
#include "render/LensFlare.hpp"
#include "render/TonemapPass.hpp"
#include "scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {

void require(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "[LensFlare] FAIL: %s\n", what);
    std::exit(1);
}

bool close(float a, float b, float tolerance = 1e-4f) { return std::abs(a - b) <= tolerance; }

saida::LightNode* addLight(saida::Scene& scene, saida::LightType type, glm::vec3 travel,
                           float intensity, bool lensFlare, bool sunStar) {
    auto light = std::make_unique<saida::LightNode>("Light", type);
    light->direction = travel;
    light->intensity = intensity;
    light->lensFlare = lensFlare;
    light->sunStar = sunStar;
    saida::LightNode* raw = light.get();
    scene.addChild(std::move(light));
    return raw;
}

// A scene that never asked keeps every existing render: both effects are off
// by default and an older document without the keys loads that way.
void testOffByDefaultAndRoundTrips() {
    saida::LightNode fresh;
    require(!fresh.lensFlare && !fresh.sunStar, "lens effects default off");

    saida::LightNode written("Sun", saida::LightType::Directional);
    written.lensFlare = true;
    written.sunStar = true;
    nlohmann::json j;
    saida::reflect::saveObject(written, j);
    require(j.at("lensFlare") == true && j.at("sunStar") == true, "both effects are saved");

    saida::LightNode read;
    saida::reflect::loadObject(read, j);
    require(read.lensFlare && read.sunStar, "both effects load back");

    j.erase("lensFlare");
    j.erase("sunStar");
    saida::LightNode older;
    saida::reflect::loadObject(older, j);
    require(!older.lensFlare && !older.sunStar, "a document without the keys keeps them off");
}

void testPicksTheBrightestDirectionalThatAsks() {
    saida::Scene none;
    addLight(none, saida::LightType::Directional, {0, -1, 0}, 5.0f, false, false);
    addLight(none, saida::LightType::Point, {0, -1, 0}, 50.0f, true, true);
    none.refreshHierarchy();
    require(!saida::pickLensFlareSource(none).active(),
            "no directional light asks: no source (a point light never flares)");

    saida::Scene scene;
    addLight(scene, saida::LightType::Directional, {1, 0, 0}, 2.0f, true, false);
    addLight(scene, saida::LightType::Directional, {0, -1, 0}, 9.0f, false, false);
    addLight(scene, saida::LightType::Directional, {0, 0, 2}, 4.0f, false, true);
    addLight(scene, saida::LightType::Directional, {0, -1, 0}, 0.0f, true, true);
    scene.refreshHierarchy();
    const saida::LensFlareSource source = saida::pickLensFlareSource(scene);
    require(source.active() && !source.lensFlare && source.sunStar,
            "the brightest light that asks wins, a brighter one that does not is ignored");
    require(close(source.towardLight.z, -1.0f) && close(glm::length(source.towardLight), 1.0f),
            "the source lies against the travel direction, normalized");
    require(close(source.radiance.r, 4.0f) && close(source.radiance.g, 4.0f),
            "radiance is colour times intensity");
}

void testProjectsTheSourceInTheShaderConvention() {
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
    const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
    const saida::SceneSettings settings;

    saida::LensFlareSource ahead;
    ahead.towardLight = {0.0f, 0.0f, -1.0f};
    ahead.radiance = {3.0f, 2.0f, 1.0f};
    ahead.lensFlare = true;
    auto push = saida::TonemapPass::pushConstants(settings, view, projection, 1.0f,
                                                  saida::rhi::Format::BGRA8Srgb, ahead);
    require(close(push.flareSource.x, 0.5f) && close(push.flareSource.y, 0.5f),
            "a source straight ahead sits at the viewport centre");
    require(push.flareSource.z == 1.0f && push.flareSource.w == 0.0f, "the flags are carried");
    require(close(push.flareRadiance.r, 3.0f) && close(push.flareRadiance.b, 1.0f),
            "the radiance is carried");

    // Up and to the right, inside the frustum: uv must invert to the same
    // view ray the shader's depth reconstruction would give.
    saida::LensFlareSource offset = ahead;
    offset.towardLight = glm::normalize(glm::vec3(0.3f, 0.2f, -1.0f));
    push = saida::TonemapPass::pushConstants(settings, view, projection, 1.0f,
                                             saida::rhi::Format::BGRA8Srgb, offset);
    const glm::vec2 ndc = glm::vec2(push.flareSource) * 2.0f - 1.0f;
    const glm::mat4 inverse = glm::inverse(projection);
    const glm::vec4 ray = inverse * glm::vec4(ndc, 0.5f, 1.0f);
    const glm::vec3 back = glm::normalize(glm::vec3(ray) / ray.w);
    require(glm::length(back - offset.towardLight) < 1e-3f,
            "the screen position inverts to the source's direction");

    saida::LensFlareSource behind = ahead;
    behind.towardLight = {0.0f, 0.0f, 1.0f};
    push = saida::TonemapPass::pushConstants(settings, view, projection, 1.0f,
                                             saida::rhi::Format::BGRA8Srgb, behind);
    require(push.flareSource == glm::vec4(0.0f) && push.flareRadiance == glm::vec4(0.0f),
            "a source behind the camera draws nothing");

    saida::LensFlareSource aside = ahead;
    aside.towardLight = glm::normalize(glm::vec3(5.0f, 0.0f, -1.0f));
    push = saida::TonemapPass::pushConstants(settings, view, projection, 1.0f,
                                             saida::rhi::Format::BGRA8Srgb, aside);
    require(push.flareSource == glm::vec4(0.0f), "a source well off screen draws nothing");

    push = saida::TonemapPass::pushConstants(settings, view, projection, 1.0f);
    require(push.flareSource == glm::vec4(0.0f), "no source, no effect");
}

} // namespace

int main() {
    testOffByDefaultAndRoundTrips();
    testPicksTheBrightestDirectionalThatAsks();
    testProjectsTheSourceInTheShaderConvention();
    std::printf("[LensFlare] PASS\n");
    return 0;
}
