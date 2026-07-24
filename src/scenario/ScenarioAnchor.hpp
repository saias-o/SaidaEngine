#pragma once

#include "core/Reflection.hpp"
#include "scene/Behaviour.hpp"

#include <string>

namespace saida {

class ScenarioAnchor : public Behaviour {
public:
    std::string key;

    SAIDA_REFLECT_BEHAVIOUR(ScenarioAnchor, "ScenarioAnchor")
};

} // namespace saida
