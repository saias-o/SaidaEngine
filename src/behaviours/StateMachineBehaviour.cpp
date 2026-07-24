#include "behaviours/StateMachineBehaviour.hpp"

#include "core/Reflection.hpp"
#include "scene/Blackboard.hpp"
#include "scene/Node.hpp"
#include "scene/SceneTree.hpp"

namespace saida {

Blackboard* StateMachineBehaviour::blackboard() const {
    if (node())
        if (Blackboard* b = node()->getBehaviour<Blackboard>()) return b;  // same node
    SceneTree* t = tree();
    if (!t) return nullptr;
    if (Blackboard* b = t->autoload<Blackboard>()) return b;
    if (Node* n = t->firstInGroup("blackboard"))
        if (Blackboard* b = n->getBehaviour<Blackboard>()) return b;
    return nullptr;
}

void StateMachineBehaviour::onReady() {
    started_ = true;
    std::string start = !initialState.empty() ? initialState
                        : (states_.empty() ? std::string() : states_.front());
    if (!start.empty()) enter(start);
}

void StateMachineBehaviour::enter(const std::string& state) {
    current_ = state;
    stateTime_ = 0.0f;
    stateChanged.emit(state);
}

bool StateMachineBehaviour::predicateHolds(const Transition& tr) const {
    const Blackboard* bb = blackboard();
    if (!bb) return false;
    double lhs = bb->number(tr.key);
    if (tr.op == "<") return lhs < tr.value;
    if (tr.op == "<=") return lhs <= tr.value;
    if (tr.op == ">") return lhs > tr.value;
    if (tr.op == ">=") return lhs >= tr.value;
    if (tr.op == "!=") return lhs != tr.value;
    return lhs == tr.value;  // default "=="
}

void StateMachineBehaviour::onUpdate(float dt) {
    if (current_.empty()) return;
    stateTime_ += dt;

    for (const auto& tr : transitions_) {
        if (tr.from != current_) continue;
        // Every declared condition must hold; trigger is consumed only on a fire.
        bool triggerOk = tr.trigger.empty() || pendingTriggers_.count(tr.trigger) != 0;
        bool predicateOk = !tr.hasPredicate || predicateHolds(tr);
        bool timeoutOk = tr.after < 0.0f || stateTime_ >= tr.after;
        if (triggerOk && predicateOk && timeoutOk) {
            if (!tr.trigger.empty()) pendingTriggers_.erase(tr.trigger);
            enter(tr.to);
            return;  // one transition per frame
        }
    }
}

void StateMachineBehaviour::fire(std::string trigger) { pendingTriggers_.insert(std::move(trigger)); }

void StateMachineBehaviour::goTo(std::string state) {
    for (const auto& s : states_)
        if (s == state) { enter(state); return; }
}

void StateMachineBehaviour::describe(reflect::TypeBuilder<StateMachineBehaviour>& t) {
    t.doc("Data-driven FSM. Configure with {initialState, states[], transitions[]} "
          "via configure_behaviour. Transitions fire on trigger/blackboard/timeout.");
    t.property("initialState", &StateMachineBehaviour::initialState).tooltip("starting state name");
    t.signal("stateChanged", &StateMachineBehaviour::stateChanged);
    t.slot("fire", &StateMachineBehaviour::fire);
    t.slot("goTo", &StateMachineBehaviour::goTo);
}

void StateMachineBehaviour::save(nlohmann::json& j) const {
    j["initialState"] = initialState;
    j["states"] = states_;
    nlohmann::json trans = nlohmann::json::array();
    for (const auto& tr : transitions_) {
        nlohmann::json t{{"from", tr.from}, {"to", tr.to}};
        if (!tr.trigger.empty()) t["trigger"] = tr.trigger;
        if (tr.hasPredicate) t["when"] = {{"key", tr.key}, {"op", tr.op}, {"value", tr.value}};
        if (tr.after >= 0.0f) t["after"] = tr.after;
        trans.push_back(std::move(t));
    }
    j["transitions"] = std::move(trans);
}

void StateMachineBehaviour::load(const nlohmann::json& j) {
    initialState = j.value("initialState", std::string());
    states_.clear();
    if (auto it = j.find("states"); it != j.end() && it->is_array())
        for (const auto& s : *it) states_.push_back(s.get<std::string>());

    transitions_.clear();
    if (auto it = j.find("transitions"); it != j.end() && it->is_array()) {
        for (const auto& t : *it) {
            Transition tr;
            tr.from = t.value("from", std::string());
            tr.to = t.value("to", std::string());
            tr.trigger = t.value("trigger", std::string());
            if (auto w = t.find("when"); w != t.end() && w->is_object()) {
                tr.hasPredicate = true;
                tr.key = w->value("key", std::string());
                tr.op = w->value("op", std::string("=="));
                tr.value = w->value("value", 0.0);
            }
            tr.after = t.value("after", -1.0f);
            transitions_.push_back(std::move(tr));
        }
    }
}

} // namespace saida
