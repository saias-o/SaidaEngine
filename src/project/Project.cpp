#include "project/Project.hpp"

#include "core/FormatVersions.hpp"
#include "core/Log.hpp"
#include "core/Paths.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace saida {

namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr const char* kProjectExtension = ".saidaproj";

// Trim leading/trailing whitespace.
std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

} // namespace

std::string resolveProjectFileInDirectory(const std::string& directory,
                                          std::string& error) {
    std::error_code ec;
    std::string found;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file(ec) ||
            entry.path().extension() != kProjectExtension)
            continue;
        if (!found.empty()) {
            error = "several .saidaproj files in " + directory +
                    "; open the intended one explicitly";
            return {};
        }
        found = entry.path().string();
    }
    if (ec) {
        error = "cannot scan " + directory + ": " + ec.message();
        return {};
    }
    if (found.empty()) {
        error = "no .saidaproj file in " + directory;
        return {};
    }
    return found;
}

bool Project::create(const std::string& parentDir, const std::string& projectName) {
    fs::path projDir = fs::path(parentDir) / projectName;

    // Create directory structure.
    try {
        fs::create_directories(projDir / "assets" / "textures");
        fs::create_directories(projDir / "assets" / "models");
        fs::create_directories(projDir / "scenes");
        fs::create_directories(projDir / "scripts");
        fs::create_directories(projDir / "shaders");
    } catch (const fs::filesystem_error& e) {
        Log::error("Project::create: failed to create directories: ", e.what());
        return false;
    }

    name_     = projectName;
    rootPath_ = projDir.string();
    filePath_ = (projDir / (projectName + kProjectExtension)).string();
    engineVersion_ = kEngineVersion;
    loaded_   = true;

    if (!save()) {
        loaded_ = false;
        return false;
    }

    assetRegistry_.sync(rootPath_);
    assetRegistry_.save(rootPath_);

    AudioManager::get().setProjectRoot(rootPath_);

    ++version_;
    Log::info("Created project '", name_, "' at ", rootPath_);
    return true;
}


bool Project::load(const std::string& neprojPath) {
    fs::path path(neprojPath);

    if (!fs::exists(path)) {
        Log::error("Project::load: file not found: ", neprojPath);
        return false;
    }

    // The Hub and --project accept the project directory as well as the
    // .saidaproj file; a directory resolves to its single project file.
    if (fs::is_directory(path)) {
        std::string resolveError;
        const std::string resolved =
            resolveProjectFileInDirectory(neprojPath, resolveError);
        if (resolved.empty()) {
            Log::error("Project::load: ", resolveError);
            return false;
        }
        return load(resolved);
    }

    std::ifstream file(neprojPath);
    if (!file.is_open()) {
        Log::error("Project::load: cannot open: ", neprojPath);
        return false;
    }

    std::string loadedName;
    std::string loadedEngineVersion;
    std::string loadedMainScene;
    int loadedMaxFps = kDefaultMaxFps;
    bool loadedVSync = kDefaultVSync;
    int loadedShadowRes = kDefaultShadowResolution;
    float loadedShadowDist = kDefaultShadowDistance;
    float loadedShadowSoft = kDefaultShadowSoftness;
    float loadedMasterVolume = 1.0f;
    AudioSettings loadedAudioSettings;
    bool loadedAutoMeshLods = false;
    bool loadedShowColliders = true;
    std::unordered_map<std::string, std::string> loadedAudioAliases;
    std::unordered_map<std::string, std::string> loadedAutoloads;

    try {
        std::stringstream buffer;
        buffer << file.rdbuf();
        const std::string text = buffer.str();
        const std::string trimmed = trim(text);

        if (trimmed.empty()) {
            Log::error("Project::load: empty project file: ", neprojPath);
            return false;
        }

        {
            json doc = json::parse(trimmed);
            if (const std::string envelope =
                    format::schemaEnvelopeError(doc, format::kProjectVersion, "project");
                !envelope.empty()) {
                Log::error("Project::load: ", envelope, ": ", neprojPath);
                return false;
            }
            loadedName = doc.value("name", std::string());
            loadedEngineVersion = doc.value("engineVersion", std::string());
            loadedMainScene = doc.value("mainScene", std::string());

            const json runtime = doc.value("runtime", json::object());
            loadedMaxFps = runtime.value("maxFps", loadedMaxFps);
            loadedVSync = runtime.value("vsync", loadedVSync);

            const json rendering = doc.value("rendering", json::object());
            loadedShadowRes = rendering.value("shadowResolution", loadedShadowRes);
            loadedShadowDist = rendering.value("shadowDistance", loadedShadowDist);
            loadedShadowSoft = rendering.value("shadowSoftness", loadedShadowSoft);
            loadedAutoMeshLods = rendering.value("autoMeshLods", loadedAutoMeshLods);
            loadedShowColliders = rendering.value("showColliders", loadedShowColliders);

            const json audio = doc.value("audio", json::object());
            loadedMasterVolume = audio.value("masterVolume", loadedMasterVolume);
            const json audioDefault = audio.value("default", json::object());
            loadedAudioSettings.volume = audioDefault.value("volume", loadedAudioSettings.volume);
            loadedAudioSettings.loop = audioDefault.value("loop", loadedAudioSettings.loop);
            loadedAudioSettings.spatialized = audioDefault.value("spatialized", loadedAudioSettings.spatialized);
            loadedAudioSettings.minDistance = audioDefault.value("minDistance", loadedAudioSettings.minDistance);
            loadedAudioSettings.maxDistance = audioDefault.value("maxDistance", loadedAudioSettings.maxDistance);

            if (auto aliases = audio.find("aliases"); aliases != audio.end() && aliases->is_object()) {
                for (const auto& [name, value] : aliases->items()) {
                    if (value.is_string()) loadedAudioAliases[name] = value.get<std::string>();
                }
            }

            if (auto autoloads = doc.find("autoloads"); autoloads != doc.end() && autoloads->is_object()) {
                for (const auto& [name, value] : autoloads->items()) {
                    if (value.is_string()) loadedAutoloads[name] = value.get<std::string>();
                }
            }
        }
    } catch (const std::exception& e) {
        Log::error("Project::load: failed to parse ", neprojPath, ": ", e.what());
        return false;
    }

    if (loadedName.empty()) {
        Log::error("Project::load: missing 'name' in project file: ", neprojPath);
        return false;
    }

    // Read the registry before committing any of this project's state, so a
    // refusal leaves the Project untouched rather than half-loaded.
    //
    // The verdict has to be honoured here: sync() below re-registers every
    // asset it finds with a brand new id, and save() writes that over the file.
    // Run on a registry the schema guard refused, the pair silently replaces
    // every AssetID a scene stored -- meshes, textures, prefabs all resolve to
    // nothing -- and the only document that could still be repaired is gone.
    // AssetRegistry::load names the exact complaint and the file it came from;
    // a missing registry is not a failure, it returns true and is created below.
    const fs::path loadedRoot = path.parent_path();
    if (!assetRegistry_.load(loadedRoot.string())) {
        Log::error("Project::load: refusing to overwrite an unreadable asset "
                   "registry with a fresh scan: ", neprojPath);
        return false;
    }

    name_          = loadedName;
    if (loadedEngineVersion.empty()) {
        Log::error("Project::load: missing 'engineVersion' in project file: ", neprojPath);
        return false;
    }

    engineVersion_ = loadedEngineVersion;
    mainScene_     = loadedMainScene;
    maxFps_        = loadedMaxFps;
    vSync_         = loadedVSync;
    shadowResolution_ = loadedShadowRes;
    shadowDistance_ = loadedShadowDist;
    shadowSoftness_ = loadedShadowSoft;
    masterVolume_ = loadedMasterVolume;
    defaultAudioSettings_ = loadedAudioSettings;
    autoMeshLods_ = loadedAutoMeshLods;
    showColliders_ = loadedShowColliders;
    audioAliases_ = std::move(loadedAudioAliases);
    autoloads_ = std::move(loadedAutoloads);
    filePath_      = path.string();
    rootPath_      = path.parent_path().string();
    loaded_        = true;
    setActiveProjectRoot(rootPath_);  // project scripts/ui resolved everywhere

    for (const auto& [aliasName, aliasPath] : audioAliases_)
        AudioManager::get().setAlias(aliasName, aliasPath);

    assetRegistry_.sync(rootPath_);
    assetRegistry_.save(rootPath_);

    AudioManager::get().setProjectRoot(rootPath_);

    ++version_;
    Log::info("Loaded project '", name_, "' from ", rootPath_);
    return true;
}


bool Project::save() const {
    if (!loaded_) {
        Log::error("Project::save: no project loaded");
        return false;
    }

    std::ofstream file(filePath_);
    if (!file.is_open()) {
        Log::error("Project::save: cannot write to ", filePath_);
        return false;
    }

    json doc;
    format::writeSchema(doc, format::kProjectVersion);
    doc["name"] = name_;
    doc["engineVersion"] = engineVersion_;
    if (!mainScene_.empty()) doc["mainScene"] = mainScene_;
    doc["runtime"] = {
        {"maxFps", maxFps_},
        {"vsync", vSync_}
    };
    doc["rendering"] = {
        {"shadowResolution", shadowResolution_},
        {"shadowDistance", shadowDistance_},
        {"shadowSoftness", shadowSoftness_},
        {"autoMeshLods", autoMeshLods_},
        {"showColliders", showColliders_}
    };

    json aliases = json::object();
    for (const auto& [aliasName, aliasPath] : audioAliases_)
        aliases[aliasName] = aliasPath;

    doc["audio"] = {
        {"masterVolume", masterVolume_},
        {"default", {
            {"volume", defaultAudioSettings_.volume},
            {"loop", defaultAudioSettings_.loop},
            {"spatialized", defaultAudioSettings_.spatialized},
            {"minDistance", defaultAudioSettings_.minDistance},
            {"maxDistance", defaultAudioSettings_.maxDistance}
        }},
        {"aliases", std::move(aliases)}
    };

    json autoloads = json::object();
    for (const auto& [autoName, autoVal] : autoloads_)
        autoloads[autoName] = autoVal;
    doc["autoloads"] = std::move(autoloads);

    file << doc.dump(2) << "\n";

    Log::info("Saved project '", name_, "' to ", filePath_);
    return true;
}

void Project::setAudioAlias(const std::string& name, const std::string& path) {
    audioAliases_[name] = path;
    AudioManager::get().setAlias(name, path);
}

void Project::removeAudioAlias(const std::string& name) {
    audioAliases_.erase(name);
    AudioManager::get().removeAlias(name);
}

void Project::setAutoload(const std::string& name, const std::string& pathOrType) {
    autoloads_[name] = pathOrType;
}

void Project::removeAutoload(const std::string& name) {
    autoloads_.erase(name);
}

} // namespace saida
