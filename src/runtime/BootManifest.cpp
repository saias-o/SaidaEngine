#include "runtime/BootManifest.hpp"

#include "core/FormatVersions.hpp"

#include <fstream>
#include <limits>

namespace saida {

namespace {

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

} // namespace

BootManifestResult parseBootManifest(std::istream& in) {
    BootManifestResult result;
    bool schemaSeen = false;

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "schema") {
            try {
                size_t consumed = 0;
                const long parsed = std::stol(value, &consumed);
                if (consumed != value.size() || parsed < 0 ||
                    parsed > std::numeric_limits<int>::max()) {
                    result.error = "boot manifest: invalid 'schema='";
                    return result;
                }
                result.manifest.schema = static_cast<int>(parsed);
                schemaSeen = true;
            } catch (...) {
                result.error = "boot manifest: invalid 'schema='";
                return result;
            }
        } else if (key == "project") result.manifest.project = value;
        else if (key == "main_scene") result.manifest.mainScene = value;
    }

    if (!schemaSeen) {
        result.error = "boot manifest: missing 'schema='";
        return result;
    }
    if (result.manifest.schema != format::kBootManifestVersion) {
        result.error = "boot manifest: unsupported schema " +
                       std::to_string(result.manifest.schema) + " (expected " +
                       std::to_string(format::kBootManifestVersion) + ")";
        return result;
    }

    if (result.manifest.project.empty()) {
        result.error = "boot manifest: missing 'project='";
        return result;
    }
    if (result.manifest.mainScene.empty()) {
        result.error = "boot manifest: missing 'main_scene='";
        return result;
    }
    result.ok = true;
    return result;
}

BootManifestResult loadBootManifest(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        BootManifestResult result;
        result.error = "boot manifest: cannot open " + path;
        return result;
    }
    return parseBootManifest(file);
}

} // namespace saida
