#pragma once

#include "Engine.hpp"
#include "core/Time.hpp"
#include <stb_image.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace saida::weather_test {

inline int checks = 0;
inline void require(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "[weather] FAIL: %s\n", message); std::abort(); }
}

struct Image {
    int width = 0, height = 0;
    std::vector<unsigned char> pixels;
};

inline Image capture(const std::filesystem::path& path, SceneSetup setup,
                     glm::vec3 cameraPos, glm::vec3 cameraTarget, float seconds = 1.25f,
                     std::function<void(unsigned)> beforeRender = {}) {
    // A GPU check must be an explicit hidden run, including when invoked by hand.
    const char* hidden = std::getenv("SAIDA_WINDOW_HIDDEN");
    require(hidden && std::string(hidden) == "1", "GPU capture requires SAIDA_WINDOW_HIDDEN=1");
    std::filesystem::create_directories(path.parent_path());
    {
        Engine engine(std::move(setup));
        engine.setCameraOverride(cameraPos, cameraTarget);
        engine.setCameraFovOverride(52.f);
        // All captures name their weather time, independently of GPU/asset latency.
        engine.setOnFrame([seconds, beforeRender = std::move(beforeRender), frame = 0u](float) mutable {
            Time::setScale(1.f);
            Time::advance(seconds - Time::elapsed());
            Time::setScale(0.f);
            if (beforeRender) beforeRender(++frame);
        });
        CaptureRequest request;
        request.pngPath = path.generic_string();
        request.frame = 3;
        request.fixedStep = 1.f / 60.f;
        engine.captureFrameThenExit(request);
        engine.run();
        require(!engine.captureFailed(), "renderer wrote weather PNG");
    }
    Time::setScale(1.f);
    Image result;
    int channels = 0;
    auto* pixels = stbi_load(path.string().c_str(), &result.width, &result.height, &channels, 4);
    require(pixels && result.width >= 64 && result.height >= 64, "weather PNG decoded");
    result.pixels.assign(pixels, pixels + size_t(result.width) * size_t(result.height) * 4u);
    stbi_image_free(pixels);
    return result;
}

struct ImageDifference {
    double meanDifference = 0, meanA = 0, meanB = 0, changedFraction = 0;
    int maxDifference = 0;
};

inline ImageDifference compare(const Image& a, const Image& b) {
    require(a.width == b.width && a.height == b.height, "capture dimensions agree");
    ImageDifference result;
    size_t changed = 0;
    const size_t count = size_t(a.width) * size_t(a.height);
    for (size_t p = 0; p < count; ++p) {
        bool differs = false;
        for (size_t c = 0; c < 3; ++c) {
            const int delta = std::abs(int(a.pixels[4 * p + c]) - int(b.pixels[4 * p + c]));
            result.meanDifference += delta;
            result.maxDifference = std::max(result.maxDifference, delta);
            result.meanA += a.pixels[4 * p + c];
            result.meanB += b.pixels[4 * p + c];
            differs |= delta > 1;
        }
        if (differs) ++changed;
    }
    result.meanDifference /= double(count) * 3.0 * 255.0;
    result.meanA /= double(count) * 3.0 * 255.0;
    result.meanB /= double(count) * 3.0 * 255.0;
    result.changedFraction = double(changed) / double(count);
    return result;
}

} // namespace saida::weather_test
