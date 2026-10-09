#pragma once

#include "WeatherRenderTestSupport.hpp"
#include "graphics/ResourceManager.hpp"
#include "scene/Scene.hpp"

#include <limits>

namespace saida::weather_test {

inline SceneSetup stormSky(float radiance, float cover, bool sunDisc = false) {
    return [=](Scene& scene, ResourceManager& resources) {
        auto& settings = scene.settings();
        settings.giEnabled = false; settings.iblEnabled = false;
        settings.bloomEnabled = false; settings.aoEnabled = false; settings.fogEnabled = false;
        settings.skyboxExposure = 1.f;
        settings.stormCloudCover = cover;
        settings.skySunDirection = glm::normalize(glm::vec3(.35f, .105f, -.94f));
        settings.skySunColor = glm::vec3(sunDisc ? 5000.f : 0.f);
        settings.skySunSize = 2.f;

        constexpr unsigned width = 64, height = 32;
        std::vector<float> pixels(width * height * 4u);
        for (size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i] = pixels[i + 1] = pixels[i + 2] = radiance;
            pixels[i + 3] = 1.f;
        }
        settings.skyboxTexture = resources.registerGeneratedTexture(
            reinterpret_cast<const uint8_t*>(pixels.data()), width, height,
            rhi::Format::RGBA32Float, true);
        require(settings.skyboxTexture != kAssetInvalid, "HDR sky fixture is resident");
    };
}

inline double skyCentreBrightness(const Image& image) {
    const int radius = std::max(1, image.height / 100);
    const int cx = image.width / 2, cy = image.height / 2;
    double sum = 0;
    size_t count = 0;
    for (int y = cy - radius; y <= cy + radius; ++y) {
        for (int x = cx - radius; x <= cx + radius; ++x) {
            const size_t offset = (size_t(y) * size_t(image.width) + size_t(x)) * 4u;
            for (size_t c = 0; c < 3; ++c) { sum += image.pixels[offset + c]; ++count; }
        }
    }
    return sum / (double(count) * 255.0);
}

inline void runStormSkyGpuTests(const std::filesystem::path& output) {
    const glm::vec3 eye(0.f), target(0.f, .48f, -1.f);
    const auto clear = capture(output / "sky-clear.png", stormSky(.4f, 0.f), eye, target);
    const auto storm = capture(output / "sky-storm.png", stormSky(.4f, 1.f), eye, target);
    const auto clouds = compare(clear, storm);
    std::printf("[weather] storm sky brightness %.4f -> %.4f, affected %.3f\n",
                clouds.meanA, clouds.meanB, clouds.changedFraction);
    require(clouds.meanA > .2, "clear HDR fixture renders above black");
    require(clouds.meanB < clouds.meanA - .04 && clouds.changedFraction > .3,
            "rain clouds visibly darken the HDR background");
    unsigned darkest = 255, brightest = 0;
    for (size_t p = 0; p < storm.pixels.size(); p += 4) {
        darkest = std::min(darkest, unsigned(storm.pixels[p]));
        brightest = std::max(brightest, unsigned(storm.pixels[p]));
    }
    require(brightest - darkest > 12, "storm sky contains cloud structure rather than a flat tint");

    SceneSettings* changingSky = nullptr;
    const auto setupChangingSky = [&](Scene& scene, ResourceManager& resources) {
        stormSky(.4f, 1.f)(scene, resources);
        changingSky = &scene.settings();
    };
    const auto returned = capture(output / "sky-returned-clear.png", setupChangingSky,
                                  eye, target, 1.25f, [&](unsigned frame) {
        require(changingSky != nullptr, "runtime sky is available before frame updates");
        if (frame >= 2) changingSky->stormCloudCover = 0.f;
    });
    require(compare(clear, returned).maxDifference == 0,
            "clearing runtime cloud cover restores the authored HDR pixels");
    const auto invalid = capture(output / "sky-invalid-cover.png",
                                stormSky(.4f, std::numeric_limits<float>::quiet_NaN()), eye, target);
    require(compare(clear, invalid).maxDifference == 0, "invalid cloud cover falls back to clear sky");

    const auto night = capture(output / "sky-night-clear.png", stormSky(.002f, 0.f), eye, target);
    const auto nightStorm = capture(output / "sky-night-storm.png", stormSky(.002f, 1.f), eye, target);
    const auto darkness = compare(night, nightStorm);
    require(darkness.meanB <= darkness.meanA + 1.0 / 255.0 && darkness.meanB < .08,
            "storm clouds retain nighttime radiance without a daytime brightness floor");

    const glm::vec3 towardSun(.35f, .105f, -.94f);
    const auto sun = capture(output / "sky-sun-clear.png", stormSky(.4f, 0.f, true), eye, towardSun);
    const auto hiddenSun = capture(output / "sky-sun-storm.png", stormSky(.4f, 1.f, true), eye, towardSun);
    const double clearSun = skyCentreBrightness(sun), coveredSun = skyCentreBrightness(hiddenSun);
    std::printf("[weather] Sun centre brightness %.4f -> %.4f\n", clearSun, coveredSun);
    require(clearSun > .95 && coveredSun < .8,
            "opaque storm clouds extinguish a high-radiance Sun instead of leaving a white hole");
}

} // namespace saida::weather_test
