#include "cli/AutoLODBridge.hpp"

#include "core/Log.hpp"
#include "core/Paths.hpp"

#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace saida {

namespace fs = std::filesystem;

std::string AutoLODBridge::exePath() {
#if defined(__EMSCRIPTEN__)
    // No external process on the web: resolveLoadPath returns the path as is
    // (the package's pre-generated _lod.glb files are still used normally).
    return {};
#elif defined(SAIDA_AUTOLOD_EXE)
    return SAIDA_AUTOLOD_EXE;
#else
    const fs::path candidate = fs::path(SAIDA_PROJECT_ROOT) / "build" / "autolod" / "autolod.exe";
    if (fs::exists(candidate)) return candidate.string();
    const fs::path alt = fs::path(SAIDA_PROJECT_ROOT) / "autolod" / "build" / "autolod.exe";
    if (fs::exists(alt)) return alt.string();
    return {};
#endif
}

bool AutoLODBridge::generate(const std::string& inputGlb, std::string& outputGlb) {
    const std::string exe = exePath();
    if (exe.empty()) {
        Log::warn("AutoLODBridge: autolod.exe not found — build the autolod target or run cmake from the repo root");
        return false;
    }

    fs::path input(inputGlb);
    fs::path output = input.parent_path() / (input.stem().string() + "_lod.glb");
    outputGlb = output.string();

    if (fs::exists(output)) {
        try {
            if (fs::last_write_time(output) >= fs::last_write_time(input))
                return true;
        } catch (const fs::filesystem_error&) {
            // fall through and regenerate
        }
    }

    const std::string cmd = "\"" + exe + "\" \"" + inputGlb + "\" \"" + outputGlb + "\"";
    Log::info("AutoLODBridge: ", cmd);
    const int ret = std::system(cmd.c_str());
    if (ret != 0) {
        Log::error("AutoLODBridge: autolod.exe exited with code ", ret);
        return false;
    }
    if (!fs::exists(output)) {
        Log::error("AutoLODBridge: expected output missing: ", outputGlb);
        return false;
    }
    Log::info("AutoLODBridge: wrote ", outputGlb);
    return true;
}

std::string AutoLODBridge::resolveLoadPath(const std::string& path, bool autoGenerate) {
    if (!autoGenerate) return path;

    fs::path p(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (ext != ".glb") return path;

    const std::string stem = p.stem().string();
    if (stem.size() >= 4 && stem.compare(stem.size() - 4, 4, "_lod") == 0)
        return path;

    std::string out;
    if (generate(path, out)) return out;
    return path;
}

} // namespace saida
