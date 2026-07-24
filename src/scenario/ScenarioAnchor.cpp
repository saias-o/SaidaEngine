#include "scenario/ScenarioAnchor.hpp"

namespace saida {

void ScenarioAnchor::describe(reflect::TypeBuilder<ScenarioAnchor>& t) {
    t.doc("Stable scenario anchor. Scenarios reference this key instead of node names or NodeIds.");
    t.property("key", &ScenarioAnchor::key).tooltip("stable scenario key");
}

} // namespace saida
