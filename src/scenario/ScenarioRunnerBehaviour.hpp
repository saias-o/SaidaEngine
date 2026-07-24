#pragma once

#include "core/Reflection.hpp"
#include "core/Signal.hpp"
#include "scene/Behaviour.hpp"
#include "scenario/ScenarioAsset.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace saida {

class Node;

class ScenarioRunnerBehaviour : public Behaviour {
public:
    ~ScenarioRunnerBehaviour() override;

    void onReady() override;
    void onUpdate(float dt) override;
    void onDestroy() override;

    void start();  // slot
    void stop();   // slot
    void pause();  // slot
    void resume(); // slot

    bool running() const { return running_; }
    bool paused() const { return paused_; }
    const std::string& currentStep() const { return currentStep_; }
    const std::string& lastError() const { return lastError_; }

    std::string scenarioPath;
    bool autoStart = true;
    bool cleanupOnStop = true;

    Signal<> started;
    Signal<std::string> stepChanged;
    Signal<std::string> finished;
    Signal<std::string> failed;

    SAIDA_REFLECT_BEHAVIOUR(ScenarioRunnerBehaviour, "ScenarioRunner")

private:
    bool loadAsset();
    std::string resolveScenarioPath() const;
    void resetRuntime();
    void enterStep(int index);
    void finish(std::string result);
    void failRun(const std::string& message);
    void updateTransitions();

    void executeCommand(const ScenarioCommand& command);
    bool evaluateCondition(const nlohmann::json& condition) const;
    void subscribeCondition(const nlohmann::json& condition);

    Node* resolveNodeRef(const nlohmann::json& ref) const;
    Node* resolveStringRef(const std::string& key) const;
    Node* findAnchor(const std::string& key) const;
    Node* firstInGroup(const std::string& group) const;
    Node* rootNode() const;

    void resolveRoles();
    bool roleMatches(const std::string& roleOrGroup, Node* candidate) const;
    std::string conditionKey(const nlohmann::json& condition) const;
    void markEvent(const nlohmann::json& condition);
    void cleanupOwned();

    ScenarioAsset asset_;
    bool loaded_ = false;
    bool running_ = false;
    bool paused_ = false;
    int currentIndex_ = -1;
    float stepElapsed_ = 0.0f;
    std::string currentStep_;
    std::string lastError_;

    std::unordered_map<std::string, nlohmann::json> blackboard_;
    std::unordered_map<std::string, Node*> roles_;
    std::unordered_set<std::string> eventFacts_;
    std::unordered_set<std::string> playedTimelines_;
    std::vector<Node*> ownedNodes_;
    std::vector<Connection> stepConnections_;
};

} // namespace saida
