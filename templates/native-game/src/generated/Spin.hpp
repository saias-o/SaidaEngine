#pragma once
#include "core/Reflection.hpp"
#include "scene/Behaviour.hpp"

namespace game {
class Spin : public saida::Behaviour {
public:
    SAIDA_REFLECT_BEHAVIOUR(Spin, "Spin")
    float speed = 0.7f;
    void onReady() override;
    void onUpdate(float dt) override;
};
} // namespace game
