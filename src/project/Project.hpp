#pragma once

#include "core/EngineVersion.hpp"
#include "project/AssetRegistry.hpp"
#include "audio/AudioManager.hpp"

#include <string>
#include <unordered_map>

namespace saida {

// Represents a SaidaEngine project on disk. A project is a directory containing
// a JSON `.saidaproj` file plus standard sub-folders (assets/, scenes/, scripts/,
// shaders/).
// The Project class manages creation, loading, and saving of this structure.
//
// When no project is loaded, `isLoaded()` returns false and the editor should
// show a welcome/project-selection screen.
class Project {
public:
    Project() = default;

    // Create a new project at `parentDir/projectName/` (creates the directory
    // structure and the .saidaproj file). Returns true on success.
    bool create(const std::string& parentDir, const std::string& projectName);

    // Load an existing project from its `.saidaproj` file path.
    // Returns true on success.
    bool load(const std::string& neprojPath);

    // Save the current project file (overwrite the .saidaproj).
    // Returns true on success.
    bool save() const;

    bool isLoaded() const { return loaded_; }

    // Bumped on every successful create()/load(). Lets the editor detect a
    // project change (even a reload of the same path) and drop stale per-project
    // state such as the undo history, which references the old scene's nodes.
    uint64_t version() const { return version_; }

    const std::string& name() const { return name_; }
    void setName(const std::string& name) { name_ = name; }
    const std::string& rootPath() const { return rootPath_; }     // project directory
    const std::string& filePath() const { return filePath_; }     // .saidaproj file
    const std::string& engineVersion() const { return engineVersion_; }

    // Project-relative path to the scene loaded at startup (e.g. "scenes/main.scene").
    // Empty means "auto-detect" (first scene found under scenes/).
    const std::string& mainScene() const { return mainScene_; }
    void setMainScene(const std::string& rel) { mainScene_ = rel; }

    AssetRegistry& assetRegistry() { return assetRegistry_; }
    const AssetRegistry& assetRegistry() const { return assetRegistry_; }

    // Standard project sub-directories (relative to rootPath).
    std::string assetsDir()  const { return rootPath_ + "/assets"; }
    std::string scenesDir()  const { return rootPath_ + "/scenes"; }
    std::string scriptsDir() const { return rootPath_ + "/scripts"; }
    std::string shadersDir() const { return rootPath_ + "/shaders"; }

    static constexpr int kDefaultMaxFps = 240;
    static constexpr bool kDefaultVSync = false;
    static constexpr int kDefaultShadowResolution = 2048;
    static constexpr float kDefaultShadowDistance = 25.0f;
    static constexpr float kDefaultShadowSoftness = 1.0f;

    int maxFps() const { return maxFps_; }
    void setMaxFps(int fps) { maxFps_ = fps; }

    bool vSync() const { return vSync_; }
    void setVSync(bool enabled) { vSync_ = enabled; }

    int shadowResolution() const { return shadowResolution_; }
    void setShadowResolution(int res) { shadowResolution_ = res; }
    
    float shadowDistance() const { return shadowDistance_; }
    void setShadowDistance(float dist) { shadowDistance_ = dist; }
    
    float shadowSoftness() const { return shadowSoftness_; }
    void setShadowSoftness(float soft) { shadowSoftness_ = soft; }

    float masterVolume() const { return masterVolume_; }
    void setMasterVolume(float vol) { masterVolume_ = vol; }

    const AudioSettings& defaultAudioSettings() const { return defaultAudioSettings_; }
    AudioSettings& defaultAudioSettings() { return defaultAudioSettings_; }

    bool autoMeshLods() const { return autoMeshLods_; }
    void setAutoMeshLods(bool enabled) { autoMeshLods_ = enabled; }

    bool showColliders() const { return showColliders_; }
    void setShowColliders(bool enabled) { showColliders_ = enabled; }

    const std::unordered_map<std::string, std::string>& audioAliases() const { return audioAliases_; }
    void setAudioAlias(const std::string& name, const std::string& path);
    void removeAudioAlias(const std::string& name);

    // Persistent singleton scenes/behaviours spawned into the World at play time
    // (Godot Autoload). Value = a ".scene" prefab path, or a registered behaviour
    // type name. Survive scene changes.
    const std::unordered_map<std::string, std::string>& autoloads() const { return autoloads_; }
    void setAutoload(const std::string& name, const std::string& pathOrType);
    void removeAutoload(const std::string& name);

private:
    bool loaded_ = false;
    uint64_t version_ = 0;
    std::string name_;
    std::string rootPath_;
    std::string filePath_;
    std::string engineVersion_ = kEngineVersion;
    std::string mainScene_;
    int maxFps_ = kDefaultMaxFps; // 0 means unlimited
    bool vSync_ = kDefaultVSync;
    int shadowResolution_ = kDefaultShadowResolution;
    float shadowDistance_ = kDefaultShadowDistance;
    float shadowSoftness_ = kDefaultShadowSoftness;
    
    float masterVolume_ = 1.0f;
    AudioSettings defaultAudioSettings_;
    bool autoMeshLods_ = false;
    bool showColliders_ = true;
    std::unordered_map<std::string, std::string> audioAliases_;
    std::unordered_map<std::string, std::string> autoloads_;

    AssetRegistry assetRegistry_;
};

// Locate the single `.saidaproj` file directly inside `directory` (used by
// Project::load for directory paths and by the Hub rename/launch flows).
// Returns an empty string and sets `error` when none or several exist.
std::string resolveProjectFileInDirectory(const std::string& directory,
                                          std::string& error);

} // namespace saida
