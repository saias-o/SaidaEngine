#pragma once

#include "core/Reflection.hpp"
#include "scene/Behaviour.hpp"

#include <string>

namespace saida {

// Optional autoload marker for projects that want a central scenario service.
// The first implementation intentionally stays tiny: actual execution is owned
// by ScenarioRunnerBehaviour instances so scenarios remain node-composable.
class ScenarioDirector : public Behaviour {
public:
    void start(std::string scenarioPath); // slot, records the requested path
    void stop();                          // slot

    const std::string& requestedScenario() const { return requestedScenario_; }
    bool hasRequest() const { return !requestedScenario_.empty(); }

    SAIDA_REFLECT_BEHAVIOUR(ScenarioDirector, "ScenarioDirector")

private:
    std::string requestedScenario_;
};

} // namespace saida
