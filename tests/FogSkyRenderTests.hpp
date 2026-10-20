#pragma once

#include "StormSkyRenderTests.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/MeshNode.hpp"

namespace saida::weather_test {

// Fog coloured by the sky (SceneSettings::fogSkyColor, SkyProbe): a wall far
// enough to vanish in the fog must turn the colour of the sky behind it, not
// the fog's own -- here a pale grey against a dark sky, the mismatch that left
// far buildings white under storm clouds.
inline SceneSetup foggedWall(float skyRadiance, float cover, float skyShare) {
    return [=](Scene& scene, ResourceManager& resources) {
        stormSky(skyRadiance, cover)(scene, resources);
        auto& settings = scene.settings();
        settings.fogEnabled = true;
        settings.fogStart = 0.f;
        settings.fogDensity = .2f;  // 60 m: transmittance e^-12, nothing of the wall left
        settings.fogColor = glm::vec4(.9f, .9f, .9f, 1.f);
        settings.fogSkyColor = skyShare;
        settings.ambientLight = glm::vec4(.5f, .5f, .5f, 1.f);
        MaterialDesc material;
        material.baseColor = glm::vec4(.8f, .2f, .2f, 1.f);
        material.doubleSided = true;
        std::vector<Vertex> vertices(4);
        const glm::vec3 positions[] = {{-80, -50, -60}, {80, -50, -60}, {80, 4, -60}, {-80, 4, -60}};
        for (int i = 0; i < 4; ++i) {
            vertices[i].pos = positions[i];
            vertices[i].normal = glm::vec3(0, 0, 1);
            vertices[i].color = glm::vec3(1);
            vertices[i].tangent = glm::vec4(1, 0, 0, 1);
        }
        const auto meshId = resources.registerMemoryMesh(vertices, {0, 1, 2, 0, 2, 3});
        scene.createChild<MeshNode>("Far wall", resources.getMesh(meshId), resources.getMaterial(material));
    };
}

// The sky's own fog (fogSkyDistance) in layered air: no geometry at all.
inline SceneSetup hazySky(float skyDistance) {
    return [=](Scene& scene, ResourceManager& resources) {
        stormSky(.05f, 0.f)(scene, resources);
        auto& settings = scene.settings();
        settings.fogEnabled = true;
        settings.fogStart = 0.f;
        settings.fogDensity = 2e-4f;
        settings.fogScaleHeight = 300.f;
        settings.fogColor = glm::vec4(.9f, .9f, .9f, 1.f);
        settings.fogSkyDistance = skyDistance;
    };
}

// Mean colour of a small square, 0..1 per channel.
inline glm::dvec3 patch(const Image& image, double fx, double fy) {
    const int cx = int(fx * image.width), cy = int(fy * image.height), r = 3;
    glm::dvec3 sum(0.0);
    int n = 0;
    for (int y = cy - r; y <= cy + r; ++y)
        for (int x = cx - r; x <= cx + r; ++x) {
            const size_t o = (size_t(y) * size_t(image.width) + size_t(x)) * 4u;
            sum += glm::dvec3(image.pixels[o], image.pixels[o + 1], image.pixels[o + 2]);
            ++n;
        }
    return sum / (double(n) * 255.0);
}

inline void runFogSkyGpuTests(const std::filesystem::path& output) {
    // Looking a little up: the sky fills the top, the wall (top edge ~4 degrees
    // up) the bottom. Wall sampled at 80 % of the height, sky at 20 %.
    const glm::vec3 eye(0.f), target(0.f, .15f, -1.f);
    const auto own = capture(output / "fog-own-colour.png", foggedWall(.05f, 0.f, 0.f), eye, target);
    const auto sky = capture(output / "fog-sky-colour.png", foggedWall(.05f, 0.f, 1.f), eye, target);
    const glm::dvec3 ownWall = patch(own, .5, .8), ownSky = patch(own, .5, .2);
    const glm::dvec3 skyWall = patch(sky, .5, .8), skySky = patch(sky, .5, .2);
    std::printf("[weather] fogged wall vs sky: own colour %.3f / %.3f, sky colour %.3f / %.3f\n",
                ownWall.g, ownSky.g, skyWall.g, skySky.g);
    require(ownWall.g > ownSky.g + .2, "without fogSkyColor the far wall takes the pale fog colour");
    require(glm::length(skyWall - skySky) < .03, "with fogSkyColor the far wall takes the sky's colour");
    require(glm::length(ownSky - skySky) < .01, "the sky itself is unchanged by the fog's colour source");

    // Storm clouds are drawn over the sky texture at render time: the fog must
    // follow them too, not the bright texture under them.
    const auto stormOwn = capture(output / "fog-storm-own.png", foggedWall(.4f, 1.f, 0.f), eye, target);
    const auto stormFromSky = capture(output / "fog-storm-sky.png", foggedWall(.4f, 1.f, 1.f), eye, target);
    const glm::dvec3 stormOwnWall = patch(stormOwn, .5, .8), stormSkyWall = patch(stormFromSky, .5, .8);
    const glm::dvec3 horizon = patch(stormFromSky, .5, .45);
    std::printf("[weather] storm: wall own %.3f, wall from sky %.3f, sky near horizon %.3f\n",
                stormOwnWall.g, stormSkyWall.g, horizon.g);
    require(stormSkyWall.g < stormOwnWall.g - .1, "under storm clouds the fogged wall darkens with the sky");
    require(std::abs(stormSkyWall.g - horizon.g) < .12, "and lands near the clouded horizon's brightness");

    // Layered air seen against the sky: a ray skimming the horizon crosses
    // kilometres of dense air, one looking 30 degrees up hardly any. The
    // horizon (just above 65 % of the height here) must go to the haze, the
    // upper sky stay itself, and a distance of 0 leave the sky untouched.
    const auto clearSky = capture(output / "sky-fog-off.png", hazySky(0.f), eye, target);
    const auto hazy = capture(output / "sky-fog-on.png", hazySky(50000.f), eye, target);
    const auto bare = capture(output / "sky-fog-none.png", stormSky(.05f, 0.f), eye, target);
    const double clearHorizon = patch(clearSky, .5, .63).g, hazyHorizon = patch(hazy, .5, .63).g;
    const double clearHigh = patch(clearSky, .5, .05).g, hazyHigh = patch(hazy, .5, .05).g;
    std::printf("[weather] sky fog: horizon %.3f -> %.3f, 30 degrees up %.3f -> %.3f\n",
                clearHorizon, hazyHorizon, clearHigh, hazyHigh);
    require(hazyHorizon > clearHorizon + .3, "the horizon band dissolves into the haze");
    require(hazyHigh < clearHigh + .15, "the sky high up stays clear");
    require(compare(clearSky, bare).maxDifference <= 1, "fogSkyDistance 0 leaves the sky as it was");
}

} // namespace saida::weather_test
