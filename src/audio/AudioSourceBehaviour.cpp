#include "audio/AudioSourceBehaviour.hpp"
#include "audio/AudioManager.hpp"
#include "project/Project.hpp"
#include "scene/Node.hpp"

#include <nlohmann/json.hpp>

namespace saida {

void AudioSourceBehaviour::onReady() {
    if (!audioName.empty()) {
        AudioManager::get().play(audioName, AudioManager::get().defaultSettings(), "Master", node());
    }
}

void AudioSourceBehaviour::describe(reflect::TypeBuilder<AudioSourceBehaviour>& t) {
    t.doc("Plays an audio alias when its node becomes ready.");
    t.property("audioName", &AudioSourceBehaviour::audioName)
        .tooltip("audio alias declared by the project");
}

} // namespace saida
