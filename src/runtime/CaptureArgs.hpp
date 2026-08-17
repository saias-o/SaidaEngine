#pragma once

#include "render/CaptureScheduler.hpp"

#include <stdexcept>
#include <string>

namespace saida::runtime {

// Where the capture is taken FROM, as opposed to which frame it lands on.
//
// A scene's own camera shows what the game shows, which is the right default and
// the wrong tool for inspecting the scene: the defects that survive a structural
// check — a wheel sunk into the road, a wall not meeting the floor — are only
// visible from somewhere the gameplay camera never goes. Overriding the
// viewpoint costs one flag pair and turns a screenshot into an instrument.
struct CaptureViewpoint {
    bool set = false;
    float position[3]{0.0f, 0.0f, 0.0f};
    float target[3]{0.0f, 0.0f, 0.0f};
};

// Parses the screenshot flags shared by the editor (src/main.cpp) and the
// standalone player (src/runtime/main.cpp). Both surfaces must accept exactly
// the same options: a golden image captured from one and compared against the
// other would otherwise be comparing two different policies.
//
// Returns false with `error` set on a malformed value. `request.pngPath` stays
// empty when no capture was asked for, which is not an error.
//
//   --screenshot <png>       write one frame to this path, then exit
//   --after-frames N         capture frame N of the settled sequence (default 2)
//   --fixed-step <seconds>   pin the frame clock (default 1/60; 0 = real clock)
//   --no-wait-assets         count frames from process start, not from settled
//   --settle-timeout N       abandon after N frames still loading (default 600)
//   --camera-pos x,y,z       put the camera here instead of the scene's own
//   --camera-look x,y,z      and aim it at this world position
inline bool parseCaptureArgs(int argc, char** argv, CaptureRequest& request,
                             CaptureViewpoint& viewpoint, std::string& error) {
    // A screenshot is asked for in order to be compared with another one, so
    // the reproducible policy is the default and the real clock is the opt-out.
    request.fixedStep = 1.0f / 60.0f;
    request.frame = 2;

    auto parseNumber = [&error](const char* text, const char* flag,
                                double& out) -> bool {
        try {
            size_t consumed = 0;
            const std::string value(text);
            out = std::stod(value, &consumed);
            if (consumed != value.size()) throw std::invalid_argument("trailing");
        } catch (const std::exception&) {
            error = std::string(flag) + " expects a number, got '" + text + "'";
            return false;
        }
        return true;
    };

    // "x,y,z" — three numbers, nothing else. A partially parsed vector is
    // refused rather than defaulted: a camera silently placed at the origin
    // photographs the inside of the floor and looks like a rendering bug.
    auto parseVec3 = [&error](const char* text, const char* flag,
                              float out[3]) -> bool {
        const std::string value(text);
        size_t start = 0;
        for (int component = 0; component < 3; ++component) {
            const size_t comma = value.find(',', start);
            const bool last = component == 2;
            if (last != (comma == std::string::npos)) {
                error = std::string(flag) + " expects x,y,z — got '" + value + "'";
                return false;
            }
            const std::string part =
                value.substr(start, last ? std::string::npos : comma - start);
            try {
                size_t consumed = 0;
                out[component] = std::stof(part, &consumed);
                if (consumed != part.size()) throw std::invalid_argument("trailing");
            } catch (const std::exception&) {
                error = std::string(flag) + " expects x,y,z — got '" + value + "'";
                return false;
            }
            start = comma + 1;
        }
        return true;
    };

    bool sawCameraPos = false;
    bool sawCameraLook = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--screenshot" && i + 1 < argc) {
            request.pngPath = argv[++i];
        } else if (arg == "--after-frames" && i + 1 < argc) {
            double value = 0.0;
            if (!parseNumber(argv[++i], "--after-frames", value)) return false;
            if (value < 1.0) {
                error = "--after-frames expects a frame number of at least 1";
                return false;
            }
            request.frame = static_cast<uint32_t>(value);
        } else if (arg == "--fixed-step" && i + 1 < argc) {
            double value = 0.0;
            if (!parseNumber(argv[++i], "--fixed-step", value)) return false;
            if (value < 0.0) {
                error = "--fixed-step expects a positive duration in seconds";
                return false;
            }
            request.fixedStep = static_cast<float>(value);
        } else if (arg == "--no-wait-assets") {
            request.waitForAssets = false;
        } else if (arg == "--settle-timeout" && i + 1 < argc) {
            double value = 0.0;
            if (!parseNumber(argv[++i], "--settle-timeout", value)) return false;
            if (value < 1.0) {
                error = "--settle-timeout expects a frame count of at least 1";
                return false;
            }
            request.settleTimeoutFrames = static_cast<uint32_t>(value);
        } else if (arg == "--camera-pos" && i + 1 < argc) {
            if (!parseVec3(argv[++i], "--camera-pos", viewpoint.position)) return false;
            sawCameraPos = true;
        } else if (arg == "--camera-look" && i + 1 < argc) {
            if (!parseVec3(argv[++i], "--camera-look", viewpoint.target)) return false;
            sawCameraLook = true;
        }
    }

    // Half a viewpoint is not a viewpoint. Accepting one alone would aim a
    // placed camera wherever the scene's camera happened to look, or place an
    // aimed one wherever the scene's camera happened to stand — either way an
    // image nobody asked for, indistinguishable from a broken scene.
    if (sawCameraPos != sawCameraLook) {
        error = "--camera-pos and --camera-look must be given together";
        return false;
    }
    viewpoint.set = sawCameraPos;
    return true;
}

} // namespace saida::runtime
