#include "scenario/ScenarioAsset.hpp"

#include "core/FormatVersions.hpp"
#include "core/Log.hpp"
#include "scenario/ScenarioRegistry.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <set>
#include <sstream>

namespace saida {
namespace {

using json = nlohmann::json;

void issue(std::vector<ScenarioIssue>& issues, std::string path, std::string message) {
    issues.push_back({std::move(path), std::move(message)});
}

std::string onlyKey(const json& object) {
    if (!object.is_object() || object.size() != 1) return {};
    return object.begin().key();
}

bool validateConditionRec(const json& condition, const std::string& path,
                          std::vector<ScenarioIssue>& issues) {
    if (condition.is_null()) return true;
    if (!condition.is_object() || condition.size() != 1) {
        issue(issues, path, "condition must be an object with exactly one key");
        return false;
    }

    const std::string name = onlyKey(condition);
    const json& args = condition.begin().value();
    if (!ScenarioConditionRegistry::isKnown(name)) {
        issue(issues, path, "unknown condition '" + name + "'");
        return false;
    }

    bool ok = true;
    if (name == "all" || name == "any") {
        if (!args.is_array()) {
            issue(issues, path + "." + name, "composite condition must be an array");
            return false;
        }
        for (std::size_t i = 0; i < args.size(); ++i)
            ok = validateConditionRec(args[i], path + "." + name + "[" + std::to_string(i) + "]", issues) && ok;
    } else if (name == "not") {
        ok = validateConditionRec(args, path + ".not", issues) && ok;
    } else if (!args.is_object() && !args.is_number() && !args.is_string()) {
        issue(issues, path + "." + name, "condition arguments must be object, number, or string");
        ok = false;
    }
    return ok;
}

bool hasTemporalOrEventCondition(const json& condition) {
    if (!condition.is_object() || condition.size() != 1) return false;
    const std::string name = onlyKey(condition);
    const json& args = condition.begin().value();
    if (name == "time.elapsed" || name == "area.entered" || name == "area.exited" ||
        name == "signal.received" || name == "input.pressed" || name == "timeline.finished") {
        return true;
    }
    if (name == "all" || name == "any") {
        if (!args.is_array()) return false;
        for (const auto& child : args)
            if (hasTemporalOrEventCondition(child)) return true;
    }
    if (name == "not") return hasTemporalOrEventCondition(args);
    return false;
}

} // namespace

bool ScenarioAsset::loadFromFile(const std::string& path, ScenarioAsset& out,
                                 std::vector<ScenarioIssue>* issues) {
    std::ifstream file(path);
    if (!file.is_open()) {
        if (issues) issues->push_back({path, "cannot open scenario file"});
        return false;
    }
    try {
        json doc = json::parse(file);
        return parse(doc, out, issues);
    } catch (const std::exception& e) {
        if (issues) issues->push_back({path, e.what()});
        return false;
    }
}

bool ScenarioAsset::parseCommand(const json& value, ScenarioCommand& out,
                                 const std::string& path, std::vector<ScenarioIssue>& issues) {
    if (!value.is_object() || value.size() != 1) {
        issue(issues, path, "action must be an object with exactly one key");
        return false;
    }
    out.name = value.begin().key();
    out.args = value.begin().value();
    return true;
}

bool ScenarioAsset::parseStep(const json& value, ScenarioStep& out,
                              const std::string& path, std::vector<ScenarioIssue>& issues) {
    if (!value.is_object()) {
        issue(issues, path, "step must be an object");
        return false;
    }
    out.id = value.value("id", std::string());
    if (auto it = value.find("enter"); it != value.end()) {
        if (!it->is_array()) issue(issues, path + ".enter", "enter must be an array");
        else {
            for (std::size_t i = 0; i < it->size(); ++i) {
                ScenarioCommand cmd;
                if (parseCommand((*it)[i], cmd, path + ".enter[" + std::to_string(i) + "]", issues))
                    out.enter.push_back(std::move(cmd));
            }
        }
    }
    if (auto it = value.find("wait"); it != value.end()) out.wait = *it;
    if (auto it = value.find("next"); it != value.end() && it->is_string()) out.next = it->get<std::string>();
    if (auto it = value.find("end"); it != value.end() && it->is_string()) out.end = it->get<std::string>();
    if (auto it = value.find("transitions"); it != value.end()) {
        if (!it->is_array()) issue(issues, path + ".transitions", "transitions must be an array");
        else {
            for (std::size_t i = 0; i < it->size(); ++i) {
                const json& tj = (*it)[i];
                ScenarioTransition tr;
                if (!tj.is_object()) {
                    issue(issues, path + ".transitions[" + std::to_string(i) + "]", "transition must be an object");
                    continue;
                }
                if (auto w = tj.find("when"); w != tj.end()) tr.when = *w;
                tr.next = tj.value("next", std::string());
                tr.end = tj.value("end", std::string());
                out.transitions.push_back(std::move(tr));
            }
        }
    }
    return true;
}

bool ScenarioAsset::parse(const json& doc, ScenarioAsset& out, std::vector<ScenarioIssue>* issues) {
    std::vector<ScenarioIssue> local;
    if (!doc.is_object()) {
        issue(local, "$", "scenario root must be an object");
    } else {
        out = ScenarioAsset{};
        if (const std::string envelope =
                format::schemaEnvelopeError(doc, format::kScenarioVersion, "scenario");
            !envelope.empty()) {
            issue(local, "$.schema", envelope);
            out.version = format::kScenarioVersion;
        } else {
            out.version = format::kScenarioVersion;
        }
        out.id = doc.value("id", std::string());
        out.blackboard = doc.value("blackboard", json::object());

        if (auto it = doc.find("roles"); it != doc.end()) {
            if (!it->is_object()) issue(local, "$.roles", "roles must be an object");
            else {
                for (auto& [name, value] : it->items())
                    out.roles[name] = ScenarioRoleDef{value};
            }
        }

        if (auto it = doc.find("steps"); it != doc.end() && it->is_array()) {
            for (std::size_t i = 0; i < it->size(); ++i) {
                ScenarioStep step;
                if (parseStep((*it)[i], step, "$.steps[" + std::to_string(i) + "]", local))
                    out.steps.push_back(std::move(step));
            }
        } else {
            issue(local, "$.steps", "steps must be an array");
        }
    }

    std::vector<ScenarioIssue> validation = out.validate();
    local.insert(local.end(), validation.begin(), validation.end());
    if (issues) *issues = local;
    return local.empty();
}

json ScenarioAsset::toJson() const {
    json doc;
    format::writeSchema(doc, format::kScenarioVersion);
    doc["id"] = id;
    json roleJson = json::object();
    for (const auto& [name, role] : roles) roleJson[name] = role.data;
    doc["roles"] = std::move(roleJson);
    doc["blackboard"] = blackboard.is_null() ? json::object() : blackboard;
    json stepJson = json::array();
    for (const auto& step : steps) {
        json sj;
        sj["id"] = step.id;
        if (!step.enter.empty()) {
            json enter = json::array();
            for (const auto& cmd : step.enter) {
                json item = json::object();
                item[cmd.name] = cmd.args;
                enter.push_back(std::move(item));
            }
            sj["enter"] = std::move(enter);
        }
        if (!step.wait.is_null()) sj["wait"] = step.wait;
        if (!step.next.empty()) sj["next"] = step.next;
        if (!step.end.empty()) sj["end"] = step.end;
        if (!step.transitions.empty()) {
            json transitions = json::array();
            for (const auto& tr : step.transitions) {
                json tj;
                if (!tr.when.is_null()) tj["when"] = tr.when;
                if (!tr.next.empty()) tj["next"] = tr.next;
                if (!tr.end.empty()) tj["end"] = tr.end;
                transitions.push_back(std::move(tj));
            }
            sj["transitions"] = std::move(transitions);
        }
        stepJson.push_back(std::move(sj));
    }
    doc["steps"] = std::move(stepJson);
    return doc;
}

std::vector<ScenarioIssue> ScenarioAsset::validate() const {
    std::vector<ScenarioIssue> issues;
    if (version != format::kScenarioVersion) issue(issues, "$.version", "unsupported scenario version");
    if (id.empty()) issue(issues, "$.id", "scenario id is required");
    if (!blackboard.is_null() && !blackboard.is_object())
        issue(issues, "$.blackboard", "blackboard must be an object");
    if (steps.empty()) issue(issues, "$.steps", "at least one step is required");

    std::set<std::string> ids;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const ScenarioStep& step = steps[i];
        const std::string path = "$.steps[" + std::to_string(i) + "]";
        if (step.id.empty()) issue(issues, path + ".id", "step id is required");
        else if (!ids.insert(step.id).second) issue(issues, path + ".id", "duplicate step id '" + step.id + "'");

        for (std::size_t a = 0; a < step.enter.size(); ++a) {
            const ScenarioCommand& cmd = step.enter[a];
            if (!ScenarioActionRegistry::isKnown(cmd.name))
                issue(issues, path + ".enter[" + std::to_string(a) + "]", "unknown action '" + cmd.name + "'");
            if (!cmd.args.is_object() && !cmd.args.is_null() && !cmd.args.is_string())
                issue(issues, path + ".enter[" + std::to_string(a) + "]", "action arguments must be object, string, or null");
        }

        if (!step.wait.is_null())
            validateConditionRec(step.wait, path + ".wait", issues);

        if (!step.next.empty() && stepIndex(step.next) < 0)
            issue(issues, path + ".next", "unknown next step '" + step.next + "'");
        if (!step.end.empty() && step.end != "success" && step.end != "failure" && step.end != "cancelled")
            issue(issues, path + ".end", "end must be success, failure, or cancelled");

        for (std::size_t t = 0; t < step.transitions.size(); ++t) {
            const auto& tr = step.transitions[t];
            const std::string tpath = path + ".transitions[" + std::to_string(t) + "]";
            if (tr.when.is_null()) issue(issues, tpath + ".when", "transition condition is required");
            else validateConditionRec(tr.when, tpath + ".when", issues);
            if (tr.next.empty() && tr.end.empty()) issue(issues, tpath, "transition needs next or end");
            if (!tr.next.empty() && stepIndex(tr.next) < 0)
                issue(issues, tpath + ".next", "unknown next step '" + tr.next + "'");
            if (!tr.end.empty() && tr.end != "success" && tr.end != "failure" && tr.end != "cancelled")
                issue(issues, tpath + ".end", "end must be success, failure, or cancelled");
        }

        if (!step.next.empty() && step.next == step.id && !hasTemporalOrEventCondition(step.wait))
            issue(issues, path + ".next", "self-loop requires an event or time condition");
    }
    return issues;
}

int ScenarioAsset::stepIndex(const std::string& stepId) const {
    for (std::size_t i = 0; i < steps.size(); ++i)
        if (steps[i].id == stepId) return static_cast<int>(i);
    return -1;
}

} // namespace saida
