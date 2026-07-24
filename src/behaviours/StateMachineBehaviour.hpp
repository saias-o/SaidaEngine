#pragma once

#include "core/ReflectionFwd.hpp"
#include "core/Signal.hpp"
#include "scene/Behaviour.hpp"

#include "nlohmann/json_fwd.hpp"

#include <set>
#include <string>
#include <vector>

namespace saida {

class Blackboard;

// Data-driven finite state machine with trigger, blackboard, and timeout transitions.
class StateMachineBehaviour : public Behaviour {
public:
    void onReady() override;
    void onUpdate(float dt) override;

    void fire(std::string trigger);  // slot: queue a one-shot trigger
    void goTo(std::string state);    // slot: force a state

    const std::string& currentState() const { return current_; }

    Signal<std::string> stateChanged;  // new state name

    std::string initialState;  // reflected property

    static constexpr const char* reflectName() { return "StateMachine"; }
    const char* typeName() const override { return "StateMachine"; }
    static void describe(reflect::TypeBuilder<StateMachineBehaviour>& t);
    void save(nlohmann::json& j) const override;
    void load(const nlohmann::json& j) override;

private:
    struct Transition {
        std::string from;
        std::string to;
        std::string trigger;       // optional one-shot trigger name
        bool hasPredicate = false; // blackboard predicate
        std::string key;
        std::string op;            // < <= > >= == !=
        double value = 0.0;
        float after = -1.0f;       // timeout in seconds (>=0 = active)
    };

    Blackboard* blackboard() const;
    void enter(const std::string& state);
    bool predicateHolds(const Transition& t) const;

    std::vector<std::string> states_;
    std::vector<Transition> transitions_;
    std::string current_;
    float stateTime_ = 0.0f;
    std::set<std::string> pendingTriggers_;
    bool started_ = false;
};

} // namespace saida
