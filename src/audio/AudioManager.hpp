#pragma once

#include <cstdint>
#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace saida {

class Node;

using AudioID = uint64_t;
constexpr AudioID kInvalidAudioID = 0;

struct AudioSettings {
    float volume = 1.0f;
    bool loop = false;
    bool spatialized = false;
    float minDistance = 1.0f;
    float maxDistance = 100.0f;
};

// Forward declaration for miniaudio internal structures without exposing them
struct AudioEngineState;
struct AudioInstance;

class AudioManager {
public:
    static AudioManager& get() {
        static AudioManager instance;
        return instance;
    }

    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    bool init();
    void shutdown();
    void update(); // Syncs node positions for spatialized sounds

    // Plays an audio file globally
    AudioID play(const std::string& audioName);
    
    // Plays an audio file with specific settings, bus, and optionally attached to a node for spatialization
    AudioID play(const std::string& audioName, const AudioSettings& settings, const std::string& bus = "Master", Node* node = nullptr);

    // Stops a specific instance of sound attached to a node, or globally if node is null
    void stop(const std::string& audioName, Node* node = nullptr);
    
    // Stops all instances of a specific sound
    void stopAll(const std::string& audioName);
    
    // Stops a specific sound by its unique ID
    void stop(AudioID id);
    
    // Immediately stops all sounds attached to a specific node. Called by Node destructor to prevent dangling pointers.
    void stopAllOnNode(Node* node);

    // Hard stops all active audio completely
    void stopAllGlobal();

    // Pausing
    void pause(const std::string& audioName);
    void pause(AudioID id);

    void setMasterVolume(float volume);
    float masterVolume() const;

    void setAlias(const std::string& name, const std::string& path) { aliases_[name] = path; }
    void removeAlias(const std::string& name) { aliases_.erase(name); }
    std::string resolveAlias(const std::string& name) const;

    // Set default project audio settings
    void setDefaultSettings(const AudioSettings& settings) { defaultSettings_ = settings; }
    const AudioSettings& defaultSettings() const { return defaultSettings_; }

    void setProjectRoot(const std::string& path) { projectRoot_ = path; }

private:
    AudioManager() = default;
    ~AudioManager() = default;

    AudioID generateID();
    
    AudioEngineState* state_ = nullptr;
    AudioSettings defaultSettings_;
    float masterVolume_ = 1.0f;
    std::string projectRoot_;
    
    std::mutex mutex_;
    
    // Tracks active instances
    std::unordered_map<AudioID, AudioInstance*> activeSounds_;
    std::unordered_map<std::string, std::string> aliases_;
    AudioID nextID_ = 1;
};

} // namespace saida
