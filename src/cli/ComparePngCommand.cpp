#include "cli/ComparePngCommand.hpp"

#include "core/PngWriter.hpp"

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace saida {

namespace {

constexpr int kExitOk = 0;
constexpr int kExitDiffers = 1;
constexpr int kExitUsage = 2;

using json = nlohmann::json;

struct Image {
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
};

bool loadPng(const std::string& path, Image& out, std::string& error) {
    int channels = 0;
    // Forced to 4 channels: the comparison is defined on RGBA, and a reference
    // saved as RGB must compare equal to the same image saved as RGBA rather
    // than being refused for a difference no viewer would show.
    stbi_uc* pixels = stbi_load(path.c_str(), &out.width, &out.height, &channels, 4);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        error = "cannot read '" + path + "': " + (reason ? reason : "unknown format");
        return false;
    }
    const size_t bytes = static_cast<size_t>(out.width) * out.height * 4;
    out.rgba.assign(pixels, pixels + bytes);
    stbi_image_free(pixels);
    return true;
}

// Reference in grey, differences in magenta. The point is not decoration: a
// human and an agent both need to see *where* the difference sits, and a raw
// per-channel delta image is unreadable at one-bit magnitudes.
std::vector<uint8_t> renderDiff(const Image& reference, const std::vector<bool>& differs) {
    std::vector<uint8_t> out(reference.rgba.size());
    for (size_t i = 0; i < differs.size(); ++i) {
        if (differs[i]) {
            out[i * 4 + 0] = 255;
            out[i * 4 + 1] = 0;
            out[i * 4 + 2] = 255;
        } else {
            const uint32_t grey = (static_cast<uint32_t>(reference.rgba[i * 4 + 0]) +
                                   reference.rgba[i * 4 + 1] + reference.rgba[i * 4 + 2]) / 3;
            // Dimmed so the magenta reads as foreground over any reference.
            const uint8_t value = static_cast<uint8_t>(grey / 3 + 24);
            out[i * 4 + 0] = value;
            out[i * 4 + 1] = value;
            out[i * 4 + 2] = value;
        }
        out[i * 4 + 3] = 255;
    }
    return out;
}

int usage(std::ostream& out) {
    out << "Usage:\n"
           "  saida_tool compare-png <actual.png> <expected.png>\n"
           "                    [--tolerance N] [--max-different N]\n"
           "                    [--diff <png>] [--json] [--pretty]\n"
           "\n"
           "  --tolerance N      per-channel absolute difference a pixel may\n"
           "                     carry and still count as equal (0-255, default 0)\n"
           "  --max-different N  number of differing pixels tolerated (default 0)\n"
           "  --diff <png>       write the reference in grey with the differing\n"
           "                     pixels in magenta\n"
           "  --json             emit the report as JSON instead of one line\n";
    return kExitUsage;
}

bool parseCount(const std::string& text, long& out) {
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || value < 0) return false;
    out = value;
    return true;
}

} // namespace

int runComparePngCommand(const std::vector<std::string>& args) {
    std::string actualPath;
    std::string expectedPath;
    std::string diffPath;
    long tolerance = 0;
    long maxDifferent = 0;
    bool asJson = false;
    bool pretty = false;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--tolerance" && i + 1 < args.size()) {
            if (!parseCount(args[++i], tolerance) || tolerance > 255) {
                std::cerr << "compare-png: --tolerance expects 0-255\n";
                return kExitUsage;
            }
        } else if (arg == "--max-different" && i + 1 < args.size()) {
            if (!parseCount(args[++i], maxDifferent)) {
                std::cerr << "compare-png: --max-different expects a count\n";
                return kExitUsage;
            }
        } else if (arg == "--diff" && i + 1 < args.size()) {
            diffPath = args[++i];
        } else if (arg == "--json") {
            asJson = true;
        } else if (arg == "--pretty") {
            pretty = true;
            asJson = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "compare-png: unknown option '" << arg << "'\n";
            return usage(std::cerr);
        } else if (actualPath.empty()) {
            actualPath = arg;
        } else if (expectedPath.empty()) {
            expectedPath = arg;
        } else {
            std::cerr << "compare-png: expects exactly two images\n";
            return usage(std::cerr);
        }
    }

    if (actualPath.empty() || expectedPath.empty()) return usage(std::cerr);

    Image actual;
    Image expected;
    std::string error;
    if (!loadPng(actualPath, actual, error) || !loadPng(expectedPath, expected, error)) {
        std::cerr << "compare-png: " << error << "\n";
        return kExitUsage;
    }

    // A size change is not a difference measured in pixels; reporting it as one
    // would invite a tolerance to hide it. It is its own verdict.
    if (actual.width != expected.width || actual.height != expected.height) {
        if (asJson) {
            json report{{"match", false},
                        {"reason", "size"},
                        {"actual", {{"width", actual.width}, {"height", actual.height}}},
                        {"expected", {{"width", expected.width}, {"height", expected.height}}}};
            std::cout << report.dump(pretty ? 2 : -1) << "\n";
        } else {
            std::cout << "compare-png: DIFFERENT size " << actual.width << "x" << actual.height
                      << " vs " << expected.width << "x" << expected.height << "\n";
        }
        return kExitDiffers;
    }

    const size_t pixelCount = static_cast<size_t>(actual.width) * actual.height;
    std::vector<bool> differs(pixelCount, false);
    size_t differentPixels = 0;
    int maxChannelDelta = 0;
    int minX = actual.width;
    int minY = actual.height;
    int maxX = -1;
    int maxY = -1;

    for (size_t i = 0; i < pixelCount; ++i) {
        int worst = 0;
        for (int c = 0; c < 4; ++c) {
            const int delta = std::abs(static_cast<int>(actual.rgba[i * 4 + c]) -
                                       static_cast<int>(expected.rgba[i * 4 + c]));
            worst = std::max(worst, delta);
        }
        maxChannelDelta = std::max(maxChannelDelta, worst);
        if (worst <= tolerance) continue;

        differs[i] = true;
        ++differentPixels;
        const int x = static_cast<int>(i % static_cast<size_t>(actual.width));
        const int y = static_cast<int>(i / static_cast<size_t>(actual.width));
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
    }

    if (!diffPath.empty()) {
        const std::vector<uint8_t> image = renderDiff(expected, differs);
        std::string writeError;
        if (!writePngRGBA8(diffPath, image.data(), static_cast<uint32_t>(actual.width),
                           static_cast<uint32_t>(actual.height), writeError)) {
            std::cerr << "compare-png: cannot write diff: " << writeError << "\n";
            return kExitUsage;
        }
    }

    const bool match = differentPixels <= static_cast<size_t>(maxDifferent);

    if (asJson) {
        json report{{"match", match},
                    {"width", actual.width},
                    {"height", actual.height},
                    {"differentPixels", differentPixels},
                    {"totalPixels", pixelCount},
                    {"maxChannelDelta", maxChannelDelta},
                    {"tolerance", tolerance},
                    {"maxDifferent", maxDifferent}};
        if (differentPixels > 0) {
            report["boundingBox"] = {{"x", minX},
                                     {"y", minY},
                                     {"width", maxX - minX + 1},
                                     {"height", maxY - minY + 1}};
        }
        if (!diffPath.empty()) report["diff"] = diffPath;
        std::cout << report.dump(pretty ? 2 : -1) << "\n";
    } else {
        std::cout << "compare-png: " << (match ? "MATCH" : "DIFFERENT") << " "
                  << differentPixels << "/" << pixelCount << " pixels, max channel delta "
                  << maxChannelDelta;
        if (differentPixels > 0) {
            std::cout << ", within " << (maxX - minX + 1) << "x" << (maxY - minY + 1)
                      << " at " << minX << "," << minY;
        }
        std::cout << "\n";
    }

    return match ? kExitOk : kExitDiffers;
}

} // namespace saida
