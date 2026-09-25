#pragma once

#include <string>

namespace saida::runtime {

// `--profile <trace.json>`: profile the whole run (Engine::profileTo). The
// flag is shared by the player and by games that host the engine themselves,
// so a trace is taken the same way whatever the executable.
//
// Returns false with `error` set when the flag has no value or the value is
// not a .json path. `tracePath` stays empty when no profile was asked for.
inline bool parseProfileArgs(int argc, char** argv, std::string& tracePath, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) != "--profile") continue;
        if (i + 1 >= argc) {
            error = "--profile needs a trace path";
            return false;
        }
        tracePath = argv[++i];
        const std::string suffix = ".json";
        if (tracePath.size() <= suffix.size() ||
            tracePath.compare(tracePath.size() - suffix.size(), suffix.size(), suffix) != 0) {
            error = "--profile writes a Chrome trace: give a .json path, not '" + tracePath + "'";
            return false;
        }
    }
    return true;
}

} // namespace saida::runtime
