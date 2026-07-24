#include "scene/animation/AnimGraphAsset.hpp"

#include "scene/animation/AnimBlackboard.hpp"
#include "scene/animation/AnimStateMachine.hpp"
#include "scene/animation/AnimationClip.hpp"
#include "scene/animation/ClipNode.hpp"

#include <cmath>
#include <deque>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace saida {

namespace {

using json = nlohmann::json;

AssetDiagnostic error(std::string code, std::string path, std::string message) {
    return {std::move(code), AssetDiagnostic::Severity::Error,
            std::move(path), std::move(message)};
}

AssetDiagnostic warning(std::string code, std::string path, std::string message) {
    return {std::move(code), AssetDiagnostic::Severity::Warning,
            std::move(path), std::move(message)};
}

bool finiteNumber(const json& j) {
    return j.is_number() && std::isfinite(j.get<double>());
}

bool validOp(const std::string& op) {
    return op == "==" || op == "!=" || op == ">" || op == "<" || op == ">=" || op == "<=";
}

ConditionOp toConditionOp(const std::string& op) {
    if (op == "==") return ConditionOp::Equals;
    if (op == "!=") return ConditionOp::NotEquals;
    if (op == ">") return ConditionOp::Greater;
    if (op == "<") return ConditionOp::Less;
    if (op == ">=") return ConditionOp::GreaterEquals;
    return ConditionOp::LessEquals;
}

const char* paramTypeName(AnimParamType t) {
    switch (t) {
        case AnimParamType::Float: return "float";
        case AnimParamType::Int: return "int";
        case AnimParamType::Bool: return "bool";
        case AnimParamType::Trigger: return "trigger";
    }
    return "float";
}

// Implicit threshold for trigger/bool conditions (shorthand {param} form).
constexpr float kBooleanConditionThreshold = 0.5f;

} // namespace

const AnimGraphClipRef* AnimGraphAsset::findClip(const std::string& alias) const {
    for (const auto& c : clips)
        if (c.alias == alias) return &c;
    return nullptr;
}

const AnimGraphState* AnimGraphAsset::findState(const std::string& stateName) const {
    for (const auto& s : states)
        if (s.name == stateName) return &s;
    return nullptr;
}

const AnimGraphParam* AnimGraphAsset::findParam(const std::string& paramName) const {
    for (const auto& p : parameters)
        if (p.name == paramName) return &p;
    return nullptr;
}

AnimGraphParseResult AnimGraphAsset::parse(const nlohmann::json& j) {
    AnimGraphParseResult result;
    auto& diags = result.diagnostics;

    if (!j.is_object()) {
        diags.push_back(error("animgraph.root.not_object", "", "document must be a JSON object"));
        return result;
    }

    const int schema = j.value("schema", 0);
    if (schema <= 0) {
        diags.push_back(error("animgraph.schema.missing", "/schema",
                              "'schema' must be a positive integer"));
        return result;
    }
    if (schema != kAnimGraphSchema) {
        diags.push_back(error("animgraph.schema.unsupported", "/schema",
                              "unsupported schema " + std::to_string(schema) +
                                  " (expected " + std::to_string(kAnimGraphSchema) + ")"));
        return result;
    }

    AnimGraphAsset& g = result.graph;
    g.name = j.value("name", "");

    if (j.contains("parameters")) {
        if (!j["parameters"].is_array()) {
            diags.push_back(error("animgraph.parameters.malformed", "/parameters",
                                  "'parameters' must be an array"));
        } else {
            size_t i = 0;
            for (const json& p : j["parameters"]) {
                const std::string path = "/parameters/" + std::to_string(i++);
                if (!p.is_object() || !p.contains("name") || !p["name"].is_string() ||
                    p["name"].get<std::string>().empty()) {
                    diags.push_back(error("animgraph.parameter.malformed", path,
                                          "parameter must be {name, type, default?}"));
                    continue;
                }
                AnimGraphParam param;
                param.name = p["name"].get<std::string>();
                const std::string type = p.value("type", "float");
                if (type == "float") param.type = AnimParamType::Float;
                else if (type == "int") param.type = AnimParamType::Int;
                else if (type == "bool") param.type = AnimParamType::Bool;
                else if (type == "trigger") param.type = AnimParamType::Trigger;
                else {
                    diags.push_back(error("animgraph.parameter.unknown_type", path + "/type",
                                          "unknown parameter type '" + type + "'"));
                    continue;
                }
                if (p.contains("default")) {
                    const json& d = p["default"];
                    if (d.is_boolean()) param.defaultValue = d.get<bool>() ? 1.0f : 0.0f;
                    else if (finiteNumber(d)) param.defaultValue = d.get<float>();
                    else {
                        diags.push_back(error("animgraph.parameter.bad_default", path + "/default",
                                              "'default' must be a finite number or boolean"));
                        continue;
                    }
                }
                g.parameters.push_back(std::move(param));
            }
        }
    }

    if (j.contains("clips")) {
        if (!j["clips"].is_object()) {
            diags.push_back(error("animgraph.clips.malformed", "/clips",
                                  "'clips' must be an object {alias: subAssetKey}"));
        } else {
            for (auto it = j["clips"].begin(); it != j["clips"].end(); ++it) {
                if (!it.value().is_string() || it.value().get<std::string>().empty()) {
                    diags.push_back(error("animgraph.clip.malformed", "/clips/" + it.key(),
                                          "clip reference must be a non-empty sub-asset key"));
                    continue;
                }
                g.clips.push_back({it.key(), it.value().get<std::string>()});
            }
        }
    }

    if (!j.contains("states") || !j["states"].is_array() || j["states"].empty()) {
        diags.push_back(error("animgraph.states.missing", "/states",
                              "'states' must be a non-empty array"));
    } else {
        size_t i = 0;
        for (const json& s : j["states"]) {
            const std::string path = "/states/" + std::to_string(i++);
            if (!s.is_object() || !s.contains("name") || !s["name"].is_string() ||
                s["name"].get<std::string>().empty() || !s.contains("play") ||
                !s["play"].is_string() || s["play"].get<std::string>().empty()) {
                diags.push_back(error("animgraph.state.malformed", path,
                                      "state must be {name, play, loop?}"));
                continue;
            }
            AnimGraphState state;
            state.name = s["name"].get<std::string>();
            state.play = s["play"].get<std::string>();
            if (s.contains("loop")) {
                if (!s["loop"].is_boolean()) {
                    diags.push_back(error("animgraph.state.bad_loop", path + "/loop",
                                          "'loop' must be a boolean"));
                    continue;
                }
                state.loop = s["loop"].get<bool>();
            }
            if (s.contains("speed")) {
                if (!finiteNumber(s["speed"])) {
                    diags.push_back(error("animgraph.state.bad_speed", path + "/speed",
                                          "'speed' must be a finite number"));
                    continue;
                }
                state.speed = s["speed"].get<float>();
            }
            g.states.push_back(std::move(state));
        }
    }

    if (!j.contains("initial") || !j["initial"].is_string() ||
        j["initial"].get<std::string>().empty()) {
        diags.push_back(error("animgraph.initial.missing", "/initial",
                              "'initial' must name the starting state"));
    } else {
        g.initial = j["initial"].get<std::string>();
    }

    if (j.contains("transitions")) {
        if (!j["transitions"].is_array()) {
            diags.push_back(error("animgraph.transitions.malformed", "/transitions",
                                  "'transitions' must be an array"));
        } else {
            size_t i = 0;
            for (const json& t : j["transitions"]) {
                const std::string path = "/transitions/" + std::to_string(i++);
                if (!t.is_object() || !t.contains("from") || !t["from"].is_string() ||
                    !t.contains("to") || !t["to"].is_string()) {
                    diags.push_back(error("animgraph.transition.malformed", path,
                                          "transition must be {from, to, when?, crossfade?}"));
                    continue;
                }
                AnimGraphTransition tr;
                tr.from = t["from"].get<std::string>();
                tr.to = t["to"].get<std::string>();
                if (t.contains("crossfade")) {
                    if (!finiteNumber(t["crossfade"]) || t["crossfade"].get<float>() < 0.0f) {
                        diags.push_back(error("animgraph.transition.bad_crossfade",
                                              path + "/crossfade",
                                              "'crossfade' must be a non-negative number"));
                        continue;
                    }
                    tr.crossfade = t["crossfade"].get<float>();
                }
                if (t.contains("exitTime")) {
                    if (!finiteNumber(t["exitTime"]) || t["exitTime"].get<float>() < 0.0f ||
                        t["exitTime"].get<float>() > 1.0f) {
                        diags.push_back(error("animgraph.transition.bad_exit_time",
                                              path + "/exitTime",
                                              "'exitTime' must be a normalized phase in [0, 1]"));
                        continue;
                    }
                    tr.exitTime = t["exitTime"].get<float>();
                }
                if (t.contains("syncPhase")) {
                    if (!t["syncPhase"].is_boolean()) {
                        diags.push_back(error("animgraph.transition.bad_sync_phase",
                                              path + "/syncPhase",
                                              "'syncPhase' must be a boolean"));
                        continue;
                    }
                    tr.syncPhase = t["syncPhase"].get<bool>();
                }
                bool badCondition = false;
                if (t.contains("when")) {
                    if (!t["when"].is_array()) {
                        diags.push_back(error("animgraph.transition.bad_when", path + "/when",
                                              "'when' must be an array of conditions"));
                        continue;
                    }
                    size_t c = 0;
                    for (const json& w : t["when"]) {
                        const std::string cpath = path + "/when/" + std::to_string(c++);
                        if (!w.is_object() || !w.contains("param") || !w["param"].is_string()) {
                            diags.push_back(error("animgraph.condition.malformed", cpath,
                                                  "condition must be {param, op?, value?}"));
                            badCondition = true;
                            continue;
                        }
                        AnimGraphCondition cond;
                        cond.param = w["param"].get<std::string>();
                        // Shorthand form {param}: "the trigger/boolean is armed".
                        if (!w.contains("op") && !w.contains("value")) {
                            cond.op = ">";
                            cond.value = kBooleanConditionThreshold;
                            tr.when.push_back(std::move(cond));
                            continue;
                        }
                        if (!w.contains("op") || !w["op"].is_string() || !w.contains("value")) {
                            diags.push_back(error("animgraph.condition.malformed", cpath,
                                                  "condition must be {param, op?, value?}"));
                            badCondition = true;
                            continue;
                        }
                        cond.op = w["op"].get<std::string>();
                        if (!validOp(cond.op)) {
                            diags.push_back(error("animgraph.condition.bad_op", cpath + "/op",
                                                  "unknown operator '" + cond.op + "'"));
                            badCondition = true;
                            continue;
                        }
                        const json& v = w["value"];
                        if (v.is_boolean()) cond.value = v.get<bool>() ? 1.0f : 0.0f;
                        else if (finiteNumber(v)) cond.value = v.get<float>();
                        else {
                            diags.push_back(error("animgraph.condition.bad_value", cpath + "/value",
                                                  "'value' must be a finite number or boolean"));
                            badCondition = true;
                            continue;
                        }
                        tr.when.push_back(std::move(cond));
                    }
                }
                if (!badCondition) g.transitions.push_back(std::move(tr));
            }
        }
    }

    result.ok = !hasErrors(diags);
    return result;
}

AnimGraphParseResult AnimGraphAsset::loadFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        AnimGraphParseResult result;
        result.diagnostics.push_back(error("animgraph.io.open", "", "cannot open " + path));
        return result;
    }
    json j = json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        AnimGraphParseResult result;
        result.diagnostics.push_back(error("animgraph.io.json", "", path + " is not valid JSON"));
        return result;
    }
    return parse(j);
}

nlohmann::json AnimGraphAsset::toJson() const {
    json j = {{"schema", kAnimGraphSchema}, {"name", name}, {"initial", initial}};

    if (!parameters.empty()) {
        json arr = json::array();
        for (const auto& p : parameters)
            arr.push_back({{"name", p.name}, {"type", paramTypeName(p.type)},
                           {"default", p.defaultValue}});
        j["parameters"] = arr;
    }
    if (!clips.empty()) {
        json obj = json::object();
        for (const auto& c : clips) obj[c.alias] = c.key;
        j["clips"] = obj;
    }
    json statesArr = json::array();
    for (const auto& s : states) {
        json sj = {{"name", s.name}, {"play", s.play}, {"loop", s.loop}};
        if (s.speed != 1.0f) sj["speed"] = s.speed;
        statesArr.push_back(std::move(sj));
    }
    j["states"] = statesArr;

    if (!transitions.empty()) {
        json arr = json::array();
        for (const auto& t : transitions) {
            json tj = {{"from", t.from}, {"to", t.to}};
            if (t.crossfade != 0.0f) tj["crossfade"] = t.crossfade;
            if (t.exitTime >= 0.0f) tj["exitTime"] = t.exitTime;
            if (t.syncPhase) tj["syncPhase"] = t.syncPhase;
            if (!t.when.empty()) {
                json when = json::array();
                for (const auto& w : t.when)
                    when.push_back({{"param", w.param}, {"op", w.op}, {"value", w.value}});
                tj["when"] = when;
            }
            arr.push_back(std::move(tj));
        }
        j["transitions"] = arr;
    }
    return j;
}

bool AnimGraphAsset::saveFile(const std::string& path) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return false;
    file << toJson().dump(1) << "\n";
    return file.good();
}

std::vector<AssetDiagnostic> AnimGraphAsset::validate() const {
    std::vector<AssetDiagnostic> diags;

    std::unordered_set<std::string> stateNames;
    for (size_t i = 0; i < states.size(); ++i) {
        if (!stateNames.insert(states[i].name).second)
            diags.push_back(error("animgraph.state.duplicate", "/states/" + std::to_string(i),
                                  "duplicate state '" + states[i].name + "'"));
        if (!findClip(states[i].play))
            diags.push_back(error("animgraph.state.unknown_clip",
                                  "/states/" + std::to_string(i) + "/play",
                                  "state '" + states[i].name + "' plays unknown clip alias '" +
                                      states[i].play + "'"));
    }

    std::unordered_set<std::string> paramNames;
    for (size_t i = 0; i < parameters.size(); ++i) {
        if (!paramNames.insert(parameters[i].name).second)
            diags.push_back(error("animgraph.parameter.duplicate",
                                  "/parameters/" + std::to_string(i),
                                  "duplicate parameter '" + parameters[i].name + "'"));
    }

    std::unordered_set<std::string> aliases;
    for (const auto& c : clips) {
        if (!aliases.insert(c.alias).second)
            diags.push_back(error("animgraph.clip.duplicate", "/clips/" + c.alias,
                                  "duplicate clip alias '" + c.alias + "'"));
    }

    if (!initial.empty() && !stateNames.count(initial))
        diags.push_back(error("animgraph.initial.unknown", "/initial",
                              "initial state '" + initial + "' does not exist"));

    // Transitions: referenced states and parameters are known.
    std::unordered_map<std::string, std::vector<std::string>> edges;
    for (size_t i = 0; i < transitions.size(); ++i) {
        const auto& t = transitions[i];
        const std::string path = "/transitions/" + std::to_string(i);
        if (!stateNames.count(t.from))
            diags.push_back(error("animgraph.transition.unknown_from", path + "/from",
                                  "transition from unknown state '" + t.from + "'"));
        if (!stateNames.count(t.to))
            diags.push_back(error("animgraph.transition.unknown_to", path + "/to",
                                  "transition to unknown state '" + t.to + "'"));
        for (size_t c = 0; c < t.when.size(); ++c) {
            if (!paramNames.count(t.when[c].param))
                diags.push_back(error("animgraph.condition.unknown_param",
                                      path + "/when/" + std::to_string(c) + "/param",
                                      "condition uses undeclared parameter '" + t.when[c].param +
                                          "'"));
        }
        edges[t.from].push_back(t.to);
    }

    // States unreachable from initial (warning).
    if (!initial.empty() && stateNames.count(initial)) {
        std::unordered_set<std::string> reached{initial};
        std::deque<std::string> frontier{initial};
        while (!frontier.empty()) {
            const std::string current = std::move(frontier.front());
            frontier.pop_front();
            for (const auto& next : edges[current])
                if (reached.insert(next).second) frontier.push_back(next);
        }
        for (size_t i = 0; i < states.size(); ++i) {
            if (!reached.count(states[i].name))
                diags.push_back(warning("animgraph.state.unreachable",
                                        "/states/" + std::to_string(i),
                                        "state '" + states[i].name +
                                            "' is unreachable from '" + initial + "'"));
        }
    }

    return diags;
}

std::unique_ptr<AnimStateMachine> AnimGraphAsset::build(
    const std::function<const AnimationClip*(const std::string& key)>& resolveClip,
    const Rig& rig, std::vector<AssetDiagnostic>* diagnostics) const {
    auto report = [&](AssetDiagnostic d) {
        if (diagnostics) diagnostics->push_back(std::move(d));
    };

    auto sm = std::make_unique<AnimStateMachine>();
    std::unordered_map<std::string, AnimState*> built;

    for (const auto& state : states) {
        const AnimGraphClipRef* ref = findClip(state.play);
        const AnimationClip* clip = ref ? resolveClip(ref->key) : nullptr;
        if (!clip) {
            report(error("animgraph.build.clip_unresolved", "/states",
                         "state '" + state.name + "': clip '" + state.play + "' (" +
                             (ref ? ref->key : "?") + ") could not be resolved"));
            continue;
        }
        auto node = std::make_unique<ClipNode>(clip, rig);
        node->setLooping(state.loop);
        node->setPlaybackSpeed(state.speed);
        auto animState = std::make_unique<AnimState>(state.name, std::move(node));
        built[state.name] = animState.get();
        sm->addState(std::move(animState));
    }

    if (built.empty()) {
        report(error("animgraph.build.empty", "", "no state could be built"));
        return nullptr;
    }

    for (const auto& t : transitions) {
        auto it = built.find(t.from);
        if (it == built.end() || !built.count(t.to)) continue;  // state not built
        AnimTransition trans;
        trans.targetState = t.to;
        trans.crossfadeDuration = t.crossfade;
        trans.exitTime = t.exitTime;
        trans.syncPhase = t.syncPhase;
        for (const auto& w : t.when) {
            const AnimGraphParam* param = findParam(w.param);
            const bool isTrigger = param && param->type == AnimParamType::Trigger;
            trans.conditions.push_back(
                {hashString(w.param), toConditionOp(w.op), w.value, isTrigger});
        }
        it->second->addTransition(std::move(trans));
    }

    if (built.count(initial)) sm->transitionTo(initial, 0.0f);

    return sm;
}

} // namespace saida
