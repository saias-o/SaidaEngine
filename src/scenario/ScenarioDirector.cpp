#include "scenario/ScenarioDirector.hpp"

namespace saida {

void ScenarioDirector::start(std::string scenarioPath) {
    requestedScenario_ = std::move(scenarioPath);
}

void ScenarioDirector::stop() {
    requestedScenario_.clear();
}

void ScenarioDirector::describe(reflect::TypeBuilder<ScenarioDirector>& t) {
    t.doc("Optional autoload marker for global scenario control. Runner behaviours execute scenarios.");
    t.slot("start", &ScenarioDirector::start);
    t.slot("stop", &ScenarioDirector::stop);
}

} // namespace saida
