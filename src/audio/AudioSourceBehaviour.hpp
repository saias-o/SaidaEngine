#pragma once

#include "core/Reflection.hpp"
#include "scene/Behaviour.hpp"
#include <string>

namespace saida {

class AudioSourceBehaviour : public Behaviour {
public:
    void onReady() override;

    std::string audioName = "default_sound";

    SAIDA_REFLECT_BEHAVIOUR(AudioSourceBehaviour, "AudioSource")
};

} // namespace saida
