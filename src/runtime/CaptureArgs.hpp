#pragma once

#include "render/CaptureScheduler.hpp"

#include <stdexcept>
#include <string>

namespace saida::runtime {

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
inline bool parseCaptureArgs(int argc, char** argv, CaptureRequest& request,
                             std::string& error) {
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
        }
    }
    return true;
}

} // namespace saida::runtime
