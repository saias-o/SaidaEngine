#include "audio/AudioManager.hpp"
#include "project/Project.hpp"
#include "project/AssetRegistry.hpp"
#include "scene/Node.hpp"
#include "core/Log.hpp"

#include "miniaudio.h"

#include <algorithm>
#include <memory>
#include <unordered_map>

namespace saida {

struct AudioEngineState {
    ma_engine engine;
    bool initialized = false;
    // Decoded once and kept (AudioManager::preload), by resolved path. The
    // engine's resource manager shares a loaded file's data with every sound
    // later initialised from the same path while one of them lives.
    std::unordered_map<std::string, std::unique_ptr<ma_sound>> resident;
};

struct AudioInstance {
    ma_sound sound;
    Node* attachedNode = nullptr;
    std::string name;
    bool isFinished = false;
};

bool AudioManager::init() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_) return true;

    state_ = new AudioEngineState();
    
    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.listenerCount = 1; // 1 listener (the camera)
    
    if (ma_engine_init(&engineConfig, &state_->engine) != MA_SUCCESS) {
        Log::error("Failed to initialize audio engine.");
        delete state_;
        state_ = nullptr;
        return false;
    }
    
    state_->initialized = true;
    ma_engine_set_volume(&state_->engine, masterVolume_);
    
    Log::info("Audio engine initialized successfully.");
    return true;
}

void AudioManager::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_ || !state_->initialized) return;

    for (auto& pair : activeSounds_) {
        ma_sound_uninit(&pair.second->sound);
        delete pair.second;
    }
    activeSounds_.clear();
    for (auto& [path, sound] : state_->resident) ma_sound_uninit(sound.get());
    state_->resident.clear();

    ma_engine_uninit(&state_->engine);
    delete state_;
    state_ = nullptr;
}

void AudioManager::update() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_ || !state_->initialized) return;

    std::vector<AudioID> toRemove;

    for (auto& pair : activeSounds_) {
        AudioID id = pair.first;
        AudioInstance* inst = pair.second;

        if (ma_sound_at_end(&inst->sound)) {
            toRemove.push_back(id);
            continue;
        }

        if (inst->attachedNode) {
            // Update 3D position
            glm::vec3 pos = glm::vec3(inst->attachedNode->worldTransform()[3]);
            ma_sound_set_position(&inst->sound, pos.x, pos.y, pos.z);
        }
    }

    for (AudioID id : toRemove) {
        ma_sound_uninit(&activeSounds_[id]->sound);
        delete activeSounds_[id];
        activeSounds_.erase(id);
    }
}

std::string AudioManager::resolveAlias(const std::string& name) const {
    std::string path = name;
    auto it = aliases_.find(name);
    if (it != aliases_.end()) {
        path = it->second; // found alias
    } else {
        if (path.find('.') == std::string::npos) {
            path += ".ogg";
        }
        if (path.find("assets") != 0 && path.find(':') == std::string::npos && path.find('/') != 0) {
            path = "assets/audio/" + path;
        }
    }
    
    // Convert to absolute path using project root if it's relative
    if (!projectRoot_.empty() && path.find(':') == std::string::npos && path.find('/') != 0) {
        path = projectRoot_ + "/" + path;
    }
    return path;
}

bool AudioManager::preload(const std::string& audioName) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_ || !state_->initialized) return false;
    const std::string filePath = resolveAlias(audioName);
    if (state_->resident.count(filePath)) return true;
    auto sound = std::make_unique<ma_sound>();
    if (ma_sound_init_from_file(&state_->engine, filePath.c_str(),
                                MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, nullptr,
                                sound.get()) != MA_SUCCESS) {
        Log::warn("Failed to preload audio: ", filePath);
        return false;
    }
    state_->resident.emplace(filePath, std::move(sound));
    return true;
}

AudioID AudioManager::play(const std::string& audioName) {
    return play(audioName, defaultSettings_, "Master", nullptr);
}

AudioID AudioManager::play(const std::string& audioName, const AudioSettings& settings,
                           const std::string& /*bus*/, Node* node) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_ || !state_->initialized) return kInvalidAudioID;

    std::string filePath = resolveAlias(audioName);
    
    AudioInstance* inst = new AudioInstance();
    inst->name = audioName;
    inst->attachedNode = node;

    ma_uint32 flags = 0;
    if (!settings.spatialized) {
        flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
    }
    // A preloaded file is shared decoded, not opened again.
    if (state_->resident.count(filePath)) flags |= MA_SOUND_FLAG_DECODE;

    ma_result result = ma_sound_init_from_file(&state_->engine, filePath.c_str(), flags, nullptr, nullptr, &inst->sound);
    if (result != MA_SUCCESS) {
        Log::warn("Failed to load audio: ", filePath);
        delete inst;
        return kInvalidAudioID;
    }

    ma_sound_set_volume(&inst->sound, settings.volume);
    ma_sound_set_looping(&inst->sound, settings.loop ? MA_TRUE : MA_FALSE);
    
    if (settings.spatialized) {
        ma_sound_set_min_distance(&inst->sound, settings.minDistance);
        ma_sound_set_max_distance(&inst->sound, settings.maxDistance);
        if (node) {
            glm::vec3 pos = glm::vec3(node->worldTransform()[3]);
            ma_sound_set_position(&inst->sound, pos.x, pos.y, pos.z);
        }
    }

    ma_sound_start(&inst->sound);

    AudioID id = generateID();
    activeSounds_[id] = inst;
    return id;
}

void AudioManager::stop(const std::string& audioName, Node* node) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = activeSounds_.begin(); it != activeSounds_.end(); ) {
        if (it->second->name == audioName && it->second->attachedNode == node) {
            ma_sound_stop(&it->second->sound);
            ma_sound_uninit(&it->second->sound);
            delete it->second;
            it = activeSounds_.erase(it);
            break; // Stop only the first one found
        } else {
            ++it;
        }
    }
}

void AudioManager::stopAll(const std::string& audioName) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = activeSounds_.begin(); it != activeSounds_.end(); ) {
        if (it->second->name == audioName) {
            ma_sound_stop(&it->second->sound);
            ma_sound_uninit(&it->second->sound);
            delete it->second;
            it = activeSounds_.erase(it);
        } else {
            ++it;
        }
    }
}

void AudioManager::stop(AudioID id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = activeSounds_.find(id);
    if (it != activeSounds_.end()) {
        ma_sound_stop(&it->second->sound);
        ma_sound_uninit(&it->second->sound);
        delete it->second;
        activeSounds_.erase(it);
    }
}

void AudioManager::stopAllOnNode(Node* node) {
    if (!node) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = activeSounds_.begin(); it != activeSounds_.end(); ) {
        if (it->second->attachedNode == node) {
            ma_sound_stop(&it->second->sound);
            ma_sound_uninit(&it->second->sound);
            delete it->second;
            it = activeSounds_.erase(it);
        } else {
            ++it;
        }
    }
}

void AudioManager::stopAllGlobal() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& pair : activeSounds_) {
        ma_sound_stop(&pair.second->sound);
        ma_sound_uninit(&pair.second->sound);
        delete pair.second;
    }
    activeSounds_.clear();
}

void AudioManager::pause(const std::string& audioName) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& pair : activeSounds_) {
        if (pair.second->name == audioName) {
            ma_sound_stop(&pair.second->sound); // in miniaudio, stop actually pauses, start resumes
        }
    }
}

void AudioManager::pause(AudioID id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = activeSounds_.find(id);
    if (it != activeSounds_.end()) {
        ma_sound_stop(&it->second->sound);
    }
}

void AudioManager::resume(AudioID id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = activeSounds_.find(id);
    if (it != activeSounds_.end()) ma_sound_start(&it->second->sound);
}

void AudioManager::setVolume(AudioID id, float volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = activeSounds_.find(id);
    if (it != activeSounds_.end()) ma_sound_set_volume(&it->second->sound, std::clamp(volume, 0.f, 1.f));
}

bool AudioManager::isPlaying(AudioID id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = activeSounds_.find(id);
    return it != activeSounds_.end() && ma_sound_is_playing(&it->second->sound);
}

void AudioManager::setMasterVolume(float volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    masterVolume_ = volume;
    if (state_ && state_->initialized) {
        ma_engine_set_volume(&state_->engine, masterVolume_);
    }
}

float AudioManager::masterVolume() const {
    return masterVolume_;
}

AudioID AudioManager::generateID() {
    return nextID_++;
}

} // namespace saida
