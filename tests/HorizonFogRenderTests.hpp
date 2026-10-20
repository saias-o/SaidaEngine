#pragma once

#include "WeatherRenderTestSupport.hpp"
#include "graphics/ResourceManager.hpp"
#include "graphics/Material.hpp"
#include "graphics/Mesh.hpp"
#include "nodes/MeshNode.hpp"
#include "scene/Scene.hpp"

namespace saida::weather_test {

inline SceneSetup horizonSky(glm::vec3 sky, glm::vec3 fog, bool enabled,
                             float cover = 0.f, bool farEdge = false, bool sun = false) {
    return [=](Scene& scene, ResourceManager& resources) {
        auto& s = scene.settings();
        s.giEnabled = s.iblEnabled = s.bloomEnabled = s.aoEnabled = false;
        s.skyboxExposure = 1.f;
        s.fogEnabled = enabled;
        s.fogStart = 0.f;
        s.fogDensity = .00001f;  // Clear air alone cannot hide a 1 km clip edge.
        s.fogColor = glm::vec4(fog, 1.f);
        s.stormCloudCover = cover;
        s.skySunDirection = {0, 0, -1};
        s.skySunColor = glm::vec3(sun ? 5000.f : 0.f);
        s.skySunSize = 1.f;
        std::vector<float> pixels(64 * 32 * 4);
        for (size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i] = sky.r; pixels[i + 1] = sky.g; pixels[i + 2] = sky.b; pixels[i + 3] = 1.f;
        }
        s.skyboxTexture = resources.registerGeneratedTexture(
            reinterpret_cast<const uint8_t*>(pixels.data()), 64, 32, rhi::Format::RGBA32Float, true);
        require(s.skyboxTexture != kAssetInvalid, "horizon HDR fixture is resident");
        if (farEdge) {
            MaterialDesc material;
            material.type = MaterialType::Unlit;
            material.baseColor = glm::vec4(.8f, .15f, .04f, 1.f);
            std::vector<Vertex> vertices(4);
            const glm::vec3 positions[] = {{-2000, -1000, -999}, {2000, -1000, -999},
                                            {2000, 0, -999}, {-2000, 0, -999}};
            for (int i = 0; i < 4; ++i) { vertices[i].pos = positions[i]; vertices[i].normal = {0, 0, 1}; }
            const auto mesh = resources.registerMemoryMesh(vertices, {0, 1, 2, 0, 2, 3});
            scene.createChild<MeshNode>("Finite terrain edge", resources.getMesh(mesh), resources.getMaterial(material));
        }
    };
}

inline int horizonPixelDelta(const Image& a, const Image& b, int x, int y) {
    int delta = 0;
    for (size_t c = 0; c < 3; ++c) {
        const size_t i = (size_t(y) * size_t(a.width) + size_t(x)) * 4 + c;
        delta = std::max(delta, std::abs(int(a.pixels[i]) - int(b.pixels[i])));
    }
    return delta;
}

inline void runHorizonFogGpuTests(const std::filesystem::path& output) {
    const glm::vec3 sky(.7f, .3f, .08f), fog(.04f, .18f, .32f), eye(0), target(0, 0, -1);
    const auto reference = capture(output / "horizon-fog-reference.png", horizonSky(fog, fog, false), eye, target);
    const auto before = capture(output / "horizon-unmatched.png", horizonSky(sky, fog, false), eye, target);
    const auto clear = capture(output / "horizon-matched.png", horizonSky(sky, fog, true), eye, target);
    const int x = clear.width / 2, y = clear.height / 2;
    require(horizonPixelDelta(before, reference, x, y) > 30, "fixture exposes a chromatic horizon mismatch");
    require(horizonPixelDelta(clear, reference, x, y) <= 1, "sky horizon matches fog RGB through HDR tonemapping");
    require(horizonPixelDelta(clear, before, x, clear.height / 8) == 0, "upper sky retains authored pixels");
    const auto storm = capture(output / "horizon-storm.png", horizonSky(sky, fog, true, 1.f), eye, target);
    require(horizonPixelDelta(storm, reference, x, y) <= 1, "horizon match is applied after storm clouds");
    const auto edge = capture(output / "horizon-terrain-edge.png", horizonSky(sky, fog, true, 0.f, true), eye, target);
    require(horizonPixelDelta(edge, reference, x, y + clear.height / 16) <= 2,
            "finite terrain reaches the sky's fog color before the far clip");
    const auto visibleEdge = capture(output / "horizon-unfogged-edge.png", horizonSky(sky, fog, false, 0.f, true), eye, target);
    require(horizonPixelDelta(visibleEdge, before, x, y + clear.height / 16) > 10,
            "terrain edge fixture actually draws geometry");
    const auto sun = capture(output / "horizon-sun.png", horizonSky(sky, fog, true, 0.f, false, true), eye, target);
    const size_t centre = (size_t(y) * size_t(sun.width) + size_t(x)) * 4;
    require(sun.pixels[centre] > 240, "horizon fade preserves the independent low Sun disc");
    const glm::vec3 pitched(0, std::tan(glm::radians(10.f)), -1);
    const auto tilted = capture(output / "horizon-pitched.png", horizonSky(sky, fog, true), eye, pitched);
    const int horizonY = std::clamp(int(y + std::tan(glm::radians(10.f)) / std::tan(glm::radians(26.f)) * y), 0, clear.height - 1);
    require(horizonPixelDelta(tilted, reference, x, horizonY) <= 1, "horizon follows world up when the camera tilts");
    const glm::vec3 nightFog(.001f, .002f, .004f);
    const auto nightRef = capture(output / "horizon-night-reference.png", horizonSky(nightFog, nightFog, false), eye, target);
    const auto night = capture(output / "horizon-night.png", horizonSky({.008f, .004f, .002f}, nightFog, true), eye, target);
    require(horizonPixelDelta(night, nightRef, x, y) <= 1, "night horizon has no daytime brightness floor");
}

} // namespace saida::weather_test
