// Locks in M5: the data-driven gameplay primitives (Blackboard, StateMachine)
// behave correctly when authored purely by data (load()) and driven
// frame by frame — the path the LLM uses via configure_behaviour.

#include "scene/Blackboard.hpp"
#include "scene/Node.hpp"
#include "scene/ReflectedTypes.hpp"
#include "behaviours/StateMachineBehaviour.hpp"

#include "nlohmann/json.hpp"

#include <cassert>
#include <string>

using json = nlohmann::json;

static void testBlackboard() {
    saida::Blackboard bb;
    std::string lastKey;
    auto conn = bb.changed.connect([&](std::string k) { lastKey = k; });

    bb.setNumber("health", 80);
    assert(lastKey == "health");
    bb.setBool("alert", true);
    assert(bb.number("health") == 80.0);
    assert(bb.boolean("alert"));
    assert(bb.number("alert") == 1.0);  // bool coerces to number

    // Round-trip through save/load.
    json saved;
    bb.save(saved);
    saida::Blackboard restored;
    restored.load(saved);
    assert(restored.number("health") == 80.0);
    assert(restored.boolean("alert"));
}

static void testStateMachinePredicateAndTimeout() {
    saida::Node npc("NPC");
    auto* bb = npc.addBehaviour<saida::Blackboard>();
    auto* sm = npc.addBehaviour<saida::StateMachineBehaviour>();

    json cfg = {
        {"initialState", "patrol"},
        {"states", {"patrol", "chase", "flee"}},
        {"transitions", json::array({
            {{"from", "patrol"}, {"to", "chase"}, {"when", {{"key", "sawPlayer"}, {"op", "=="}, {"value", 1}}}},
            {{"from", "chase"}, {"to", "flee"}, {"after", 0.5}}})}};
    sm->load(cfg);

    std::string state;
    auto conn = sm->stateChanged.connect([&](std::string s) { state = s; });

    sm->onReady();
    assert(state == "patrol");

    sm->onUpdate(0.1f);  // predicate not yet true
    assert(sm->currentState() == "patrol");

    bb->setNumber("sawPlayer", 1);
    sm->onUpdate(0.1f);  // predicate holds -> chase
    assert(sm->currentState() == "chase");

    sm->onUpdate(0.4f);  // 0.4 < 0.5 timeout
    assert(sm->currentState() == "chase");
    sm->onUpdate(0.2f);  // 0.6 >= 0.5 -> flee
    assert(sm->currentState() == "flee");
}

static void testStateMachineTrigger() {
    saida::Node n("G");
    auto* sm = n.addBehaviour<saida::StateMachineBehaviour>();
    sm->load({{"initialState", "idle"},
              {"states", {"idle", "go"}},
              {"transitions", json::array({{{"from", "idle"}, {"to", "go"}, {"trigger", "jump"}}})}});
    sm->onReady();
    sm->onUpdate(0.1f);
    assert(sm->currentState() == "idle");  // no trigger yet
    sm->fire("jump");
    sm->onUpdate(0.1f);
    assert(sm->currentState() == "go");
}

int main() {
    saida::registerReflectedTypes();
    testBlackboard();
    testStateMachinePredicateAndTimeout();
    testStateMachineTrigger();
    return 0;
}
