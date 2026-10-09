#include "WeatherRenderTestSupport.hpp"
#include "StormSkyRenderTests.hpp"
#include "graphics/Material.hpp"
#include "graphics/Mesh.hpp"
#include "graphics/ResourceManager.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/MeshNode.hpp"
#include "render/RainWeather.hpp"
#include "render/Renderer.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSettingsSerialization.hpp"
#include <nlohmann/json.hpp>
#include <cstddef>
#include <limits>
#include <unordered_set>

namespace {
using namespace saida;
using namespace saida::weather_test;

void contracts() {
    const SceneSettings defaults;
    require(defaults.rainIntensity == 0.f && defaults.wetness == 0.f &&
            defaults.stormCloudCover == 0.f, "default weather preserves existing scenes");
    require(RainSurface{}.reception == 0.f, "materials opt in to rain");
    std::unordered_set<MaterialDesc> materials;
    MaterialDesc dry;
    materials.insert(dry);
    MaterialDesc opted = dry; opted.rain.reception = 1.f;
    materials.insert(opted);
    auto changed = opted; changed.rain.puddleAmount = .9f; materials.insert(changed);
    changed = opted; changed.rain.rippleStrength = .7f; materials.insert(changed);
    changed = opted; changed.rain.wetDarkening = .8f; materials.insert(changed);
    require(materials.size() == 5, "material cache preserves each rain response");
    require(std::hash<MaterialDesc>{}(opted) == std::hash<MaterialDesc>{}(MaterialDesc(opted)),
            "equal weather materials have equal hashes");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    RainSurface invalid; invalid.reception = nan; invalid.puddleAmount = 5.f;
    invalid.rippleStrength = -2.f; invalid.wetDarkening = std::numeric_limits<float>::infinity();
    require(invalid.parameters() == glm::vec4(0, 1, 0, 0), "material weather uploads finite unit values");
    auto weather = packRainWeather(2.f, -.5f, glm::vec3(0, 3, 4), 17.f);
    require(weather.params == glm::vec4(1, 0, 17, 0), "weather uploads clamp intensity and wetness");
    require(glm::length(glm::vec3(weather.up) - glm::vec3(0, .6f, .8f)) < 1e-6f,
            "rain frame normalizes planetary up");
    weather = packRainWeather(nan, nan, glm::vec3(nan), nan);
    require(weather.params == glm::vec4(0) && weather.up == glm::vec4(0, 1, 0, 0),
            "invalid weather uses dry state and finite up");
    require(packRainWeather(1, 1, glm::vec3(0), -1).up == glm::vec4(0, 1, 0, 0),
            "zero rain direction has defined fallback");
    require(sizeof(RainWeatherUniforms) == 32 && offsetof(LightingUBO, rainUp) ==
            offsetof(LightingUBO, rainParams) + 16 && sizeof(LightingUBO) % 16 == 0,
            "shared mono XR Web rain block respects vec4 layout");
    SceneSettings transient;
    transient.rainIntensity = 1; transient.wetness = .7f; transient.stormCloudCover = .8f;
    transient.rainUpDirection = glm::vec3(0, 0, 1);
    nlohmann::json document;
    writeSceneSettings(transient, document);
    require(!document.contains("rainIntensity") && !document.contains("wetness") &&
            !document.contains("rainUpDirection") && !document.contains("stormCloudCover"),
            "runtime weather does not expand the authoring format");
}

SceneSetup surface(bool wall, bool opted, float intensity, float wetness,
                   float ripple = .42f, MaterialType type = MaterialType::Lit) {
    return [=](Scene& scene, ResourceManager& resources) {
        auto& settings = scene.settings();
        settings.giEnabled = false; settings.iblEnabled = false;
        settings.bloomEnabled = false; settings.aoEnabled = false; settings.fogEnabled = false;
        settings.ambientLight = glm::vec4(.18f, .18f, .18f, 1);
        settings.clearColor = glm::vec4(.025f, .03f, .04f, 1);
        settings.rainIntensity = intensity; settings.wetness = wetness;
        MaterialDesc material;
        material.baseColor = glm::vec4(.38f, .42f, .46f, 1);
        material.roughness = .82f; material.doubleSided = true; material.type = type;
        material.rain.reception = opted ? 1.f : 0.f;
        material.rain.puddleAmount = .90f; material.rain.rippleStrength = ripple;
        material.rain.wetDarkening = .35f;
        if (wall) {
            // The mapped normal leans strongly upward, but admission must use
            // the geometric wall, independent of this decorative relief.
            const uint8_t normal[] = {128, 245, 177, 255};
            material.normalId = resources.registerGeneratedTexture(normal, 1, 1,
                                                  rhi::Format::RGBA8Unorm, false);
        }
        std::vector<Vertex> vertices(4);
        const glm::vec3 positions[] = {{-5, 0, -5}, {5, 0, -5}, {5, 0, 5}, {-5, 0, 5}};
        const glm::vec3 wallPositions[] = {{-5, -5, 0}, {5, -5, 0}, {5, 5, 0}, {-5, 5, 0}};
        const glm::vec2 uv[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (int i = 0; i < 4; ++i) {
            vertices[i].pos = wall ? wallPositions[i] : positions[i];
            vertices[i].normal = wall ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
            vertices[i].color = glm::vec3(1); vertices[i].texCoord = uv[i];
            vertices[i].lightmapUV = uv[i]; vertices[i].tangent = glm::vec4(1, 0, 0, 1);
        }
        const std::vector<uint32_t> indices = wall ? std::vector<uint32_t>{0, 1, 2, 0, 2, 3}
                                                   : std::vector<uint32_t>{0, 2, 1, 0, 3, 2};
        auto meshId = resources.registerMemoryMesh(vertices, indices);
        scene.createChild<MeshNode>("Rain receiver", resources.getMesh(meshId), resources.getMaterial(material));
        auto* sun = scene.createChild<LightNode>();
        sun->castShadows = false; sun->intensity = 2.2f;
        sun->direction = glm::normalize(glm::vec3(0, -.55f, .84f));
    };
}

void gpu(const std::filesystem::path& output) {
    const glm::vec3 eye(0, 2.2f, 4), target(0, 0, 0);
    auto dry = capture(output / "ground-dry.png", surface(false, true, 0, 0), eye, target);
    auto wet = capture(output / "ground-wet.png", surface(false, true, 0, 1), eye, target);
    auto damp = compare(dry, wet);
    std::printf("[weather] ground wet delta %.6f, affected %.3f\n", damp.meanDifference, damp.changedFraction);
    require(damp.meanDifference > .01 && damp.changedFraction > .2, "wet receiver changes visible shading");
    auto rainA = capture(output / "ground-rain-a.png", surface(false, true, 1, 1), eye, target, 1.25f);
    auto rainB = capture(output / "ground-rain-b.png", surface(false, true, 1, 1), eye, target, 1.68f);
    auto impacts = compare(wet, rainA), motion = compare(rainA, rainB);
    std::printf("[weather] impacts delta %.6f, animated %.6f / %.3f\n",
                impacts.meanDifference, motion.meanDifference, motion.changedFraction);
    require(impacts.meanDifference > .0005 && impacts.maxDifference > 8, "rain impacts are visible on wet ground");
    require(motion.meanDifference > .0005 && motion.changedFraction > .01, "radial impacts animate at fixed world positions");
    auto offDry = capture(output / "optout-dry.png", surface(false, false, 0, 0), eye, target);
    auto offRain = capture(output / "optout-rain.png", surface(false, false, 1, 1), eye, target);
    require(compare(offDry, offRain).maxDifference == 0, "opt-out surface stays pixel-identical in rain");
    auto wallDry = capture(output / "wall-dry.png", surface(true, true, 0, 0), {0, 0, 5}, target);
    auto wallRain = capture(output / "wall-rain.png", surface(true, true, 1, 1), {0, 0, 5}, target);
    require(compare(wallDry, wallRain).maxDifference == 0, "vertical opted-in walls do not receive puddles or impacts");
    auto unlitDry = capture(output / "unlit-dry.png", surface(false, true, 0, 0, .42f, MaterialType::Unlit), eye, target);
    auto unlitRain = capture(output / "unlit-rain.png", surface(false, true, 1, 1, .42f, MaterialType::Unlit), eye, target);
    require(compare(unlitDry, unlitRain).maxDifference == 0, "unlit surfaces preserve their authored pixels");
}
} // namespace

int main(int argc, char** argv) {
    contracts();
    if (argc >= 2 && std::string(argv[1]) == "--gpu") {
        const std::filesystem::path output = argc >= 3 ? argv[2] : "build/weather-captures";
        gpu(output);
        saida::weather_test::runStormSkyGpuTests(output);
    }
    std::printf("[weather] PASS (%d checks)\n", saida::weather_test::checks);
}
