#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace saida {

struct ScenarioIssue {
    std::string path;
    std::string message;
};

struct ScenarioCommand {
    std::string name;
    nlohmann::json args;
};

struct ScenarioTransition {
    nlohmann::json when;
    std::string next;
    std::string end;
};

struct ScenarioStep {
    std::string id;
    std::vector<ScenarioCommand> enter;
    nlohmann::json wait;
    std::string next;
    std::string end;
    std::vector<ScenarioTransition> transitions;
};

struct ScenarioRoleDef {
    nlohmann::json data;
};

class ScenarioAsset {
public:
    int version = 1;
    std::string id;
    std::unordered_map<std::string, ScenarioRoleDef> roles;
    nlohmann::json blackboard;
    std::vector<ScenarioStep> steps;

    static bool loadFromFile(const std::string& path, ScenarioAsset& out,
                             std::vector<ScenarioIssue>* issues = nullptr);
    static bool parse(const nlohmann::json& doc, ScenarioAsset& out,
                      std::vector<ScenarioIssue>* issues = nullptr);

    nlohmann::json toJson() const;
    std::vector<ScenarioIssue> validate() const;
    int stepIndex(const std::string& stepId) const;

private:
    static bool parseCommand(const nlohmann::json& value, ScenarioCommand& out,
                             const std::string& path, std::vector<ScenarioIssue>& issues);
    static bool parseStep(const nlohmann::json& value, ScenarioStep& out,
                          const std::string& path, std::vector<ScenarioIssue>& issues);
};

} // namespace saida
