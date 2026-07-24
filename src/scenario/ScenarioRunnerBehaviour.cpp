#include "scenario/ScenarioRunnerBehaviour.hpp"

#include "core/Input.hpp"
#include "core/Log.hpp"
#include "core/Reflection.hpp"
#include "physics/AreaNode.hpp"
#include "scene/Node.hpp"
#include "scene/SceneTree.hpp"
#include "scenario/ScenarioAnchor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace saida {
namespace {

using json = nlohmann::json;

glm::vec3 vec3Arg(const json& j, const glm::vec3& fallback) {
    if (j.is_array() && j.size() == 3)
        return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
    return fallback;
}

bool jsonEquals(const json& a, const json& b) {
    if (a.is_number() && b.is_number())
        return std::abs(a.get<double>() - b.get<double>()) < 0.000001;
    return a == b;
}

bool compareCount(int lhs, const std::string& op, int rhs) {
    if (op == "<") return lhs < rhs;
    if (op == "<=") return lhs <= rhs;
    if (op == ">") return lhs > rhs;
    if (op == ">=") return lhs >= rhs;
    if (op == "!=") return lhs != rhs;
    return lhs == rhs;
}

struct SignalHit { void* obj = nullptr; const reflect::SignalDesc* desc = nullptr; };
struct SlotHit { void* obj = nullptr; const reflect::SlotDesc* desc = nullptr; };

SignalHit findSignal(Node* node, const std::string& name) {
    if (!node) return {};
    auto& reg = reflect::TypeRegistry::instance();
    if (const auto* d = reg.find(node->typeName()))
        if (const auto* s = d->findSignal(name)) return {node, s};
    for (const auto& b : node->behaviours())
        if (b->typeName())
            if (const auto* d = reg.find(b->typeName()))
                if (const auto* s = d->findSignal(name)) return {b.get(), s};
    return {};
}

SlotHit findSlot(Node* node, const std::string& name) {
    if (!node) return {};
    auto& reg = reflect::TypeRegistry::instance();
    if (const auto* d = reg.find(node->typeName()))
        if (const auto* s = d->findSlot(name)) return {node, s};
    for (const auto& b : node->behaviours())
        if (b->typeName())
            if (const auto* d = reg.find(b->typeName()))
                if (const auto* s = d->findSlot(name)) return {b.get(), s};
    return {};
}

std::string singleKey(const json& object) {
    if (!object.is_object() || object.size() != 1) return {};
    return object.begin().key();
}

json commandArgs(const ScenarioCommand& command) {
    return command.args.is_null() ? json::object() : command.args;
}

} // namespace

ScenarioRunnerBehaviour::~ScenarioRunnerBehaviour() = default;

void ScenarioRunnerBehaviour::onReady() {
    if (autoStart) start();
}

void ScenarioRunnerBehaviour::onDestroy() {
    resetRuntime();
}

void ScenarioRunnerBehaviour::onUpdate(float dt) {
    if (!running_ || paused_) return;
    stepElapsed_ += dt;
    updateTransitions();
}

void ScenarioRunnerBehaviour::start() {
    lastError_.clear();
    if (!loadAsset()) {
        failRun(lastError_.empty() ? "failed to load scenario" : lastError_);
        return;
    }
    resetRuntime();
    if (asset_.blackboard.is_object()) {
        for (auto& [key, value] : asset_.blackboard.items())
            blackboard_[key] = value;
    }
    running_ = true;
    paused_ = false;
    resolveRoles();
    if (!lastError_.empty()) {
        failRun(lastError_);
        return;
    }
    started.emit();
    enterStep(0);
}

void ScenarioRunnerBehaviour::stop() {
    if (!running_) return;
    finish("cancelled");
}

void ScenarioRunnerBehaviour::pause() {
    if (running_) paused_ = true;
}

void ScenarioRunnerBehaviour::resume() {
    if (running_) paused_ = false;
}

bool ScenarioRunnerBehaviour::loadAsset() {
    std::vector<ScenarioIssue> issues;
    loaded_ = ScenarioAsset::loadFromFile(resolveScenarioPath(), asset_, &issues);
    if (!loaded_) {
        lastError_.clear();
        for (const auto& issue : issues) {
            if (!lastError_.empty()) lastError_ += "; ";
            lastError_ += issue.path + ": " + issue.message;
        }
    }
    return loaded_;
}

std::string ScenarioRunnerBehaviour::resolveScenarioPath() const {
    std::filesystem::path path(scenarioPath);
    if (path.is_absolute()) return path.string();
    if (node())
        if (SceneTree* t = node()->tree()) return t->resolveProjectPath(scenarioPath);
    if (std::filesystem::exists(path)) return std::filesystem::absolute(path).string();
    return (std::filesystem::path(SAIDA_PROJECT_ROOT) / path).string();
}

void ScenarioRunnerBehaviour::resetRuntime() {
    stepConnections_.clear();
    eventFacts_.clear();
    roles_.clear();
    blackboard_.clear();
    playedTimelines_.clear();
    lastError_.clear();
    currentIndex_ = -1;
    currentStep_.clear();
    stepElapsed_ = 0.0f;
    if (cleanupOnStop) cleanupOwned();
}

void ScenarioRunnerBehaviour::enterStep(int index) {
    if (!running_) return;
    if (index < 0 || index >= static_cast<int>(asset_.steps.size())) {
        finish("success");
        return;
    }

    stepConnections_.clear();
    eventFacts_.clear();
    stepElapsed_ = 0.0f;
    currentIndex_ = index;
    const ScenarioStep& step = asset_.steps[static_cast<std::size_t>(index)];
    currentStep_ = step.id;
    stepChanged.emit(currentStep_);

    for (const ScenarioCommand& command : step.enter) executeCommand(command);
    if (!running_) return;

    if (!step.wait.is_null()) subscribeCondition(step.wait);
    for (const ScenarioTransition& tr : step.transitions)
        if (!tr.when.is_null()) subscribeCondition(tr.when);

    updateTransitions();
}

void ScenarioRunnerBehaviour::finish(std::string result) {
    if (!running_) return;
    running_ = false;
    paused_ = false;
    stepConnections_.clear();
    eventFacts_.clear();
    if (cleanupOnStop) cleanupOwned();
    finished.emit(std::move(result));
}

void ScenarioRunnerBehaviour::failRun(const std::string& message) {
    lastError_ = message;
    running_ = false;
    paused_ = false;
    stepConnections_.clear();
    if (cleanupOnStop) cleanupOwned();
    failed.emit(lastError_);
    Log::warn("ScenarioRunner: ", lastError_);
}

void ScenarioRunnerBehaviour::updateTransitions() {
    if (!running_ || currentIndex_ < 0 || currentIndex_ >= static_cast<int>(asset_.steps.size())) return;
    const ScenarioStep& step = asset_.steps[static_cast<std::size_t>(currentIndex_)];

    for (const ScenarioTransition& tr : step.transitions) {
        if (!tr.when.is_null() && evaluateCondition(tr.when)) {
            if (!tr.end.empty()) { finish(tr.end); return; }
            enterStep(asset_.stepIndex(tr.next));
            return;
        }
    }

    if (!step.transitions.empty()) return;
    if (!step.wait.is_null() && !evaluateCondition(step.wait)) return;
    if (!step.end.empty()) { finish(step.end); return; }
    if (!step.next.empty()) { enterStep(asset_.stepIndex(step.next)); return; }
    finish("success");
}

void ScenarioRunnerBehaviour::executeCommand(const ScenarioCommand& command) {
    const json args = commandArgs(command);

    if (command.name == "blackboard.set") {
        std::string key = args.value("key", std::string());
        if (!key.empty()) blackboard_[key] = args.contains("value") ? args["value"] : json();
        return;
    }
    if (command.name == "node.enable" || command.name == "node.disable") {
        Node* target = resolveNodeRef(args.contains("target") ? args["target"] : args);
        if (target) target->setEnabled(command.name == "node.enable");
        return;
    }
    if (command.name == "node.setTransform") {
        Node* target = resolveNodeRef(args.contains("target") ? args["target"] : args);
        if (!target) return;
        if (args.contains("position")) target->transform().position = vec3Arg(args["position"], target->transform().position);
        if (args.contains("scale")) target->transform().scale = vec3Arg(args["scale"], target->transform().scale);
        return;
    }
    if (command.name == "scene.instantiate") {
        if (!node() || !node()->tree()) return;
        std::string scene = args.value("scene", std::string());
        if (scene.empty() && args.is_string()) scene = args.get<std::string>();
        if (scene.empty()) return;
        Node* parent = args.contains("parent") ? resolveNodeRef(args["parent"]) : nullptr;
        Node* spawned = node()->tree()->instantiate(scene, parent);
        if (!spawned) return;
        ownedNodes_.push_back(spawned);
        if (args.contains("at")) {
            if (Node* at = resolveNodeRef(args["at"]))
                spawned->transform().position = glm::vec3(at->worldTransform()[3]);
        }
        std::string role = args.value("role", std::string());
        if (!role.empty()) roles_[role] = spawned;
        return;
    }
    if (command.name == "scene.freeOwned") {
        cleanupOwned();
        return;
    }
    if (command.name == "signal.emit") {
        Node* target = resolveNodeRef(args.contains("target") ? args["target"] : args);
        std::string signal = args.value("signal", std::string());
        SignalHit hit = findSignal(target, signal);
        if (hit.desc && hit.desc->emit)
            hit.desc->emit(hit.obj, args.value("args", json::array()));
        return;
    }
    if (command.name == "slot.call") {
        Node* target = resolveNodeRef(args.contains("target") ? args["target"] : args);
        std::string slot = args.value("slot", std::string());
        SlotHit hit = findSlot(target, slot);
        if (hit.desc) hit.desc->invoke(hit.obj, args.value("args", json::array()));
        return;
    }
    if (command.name == "timeline.play") {
        std::string name = args.value("name", args.value("timeline", std::string()));
        if (name.empty() && args.is_string()) name = args.get<std::string>();
        if (!name.empty()) playedTimelines_.insert(name);
        return;
    }
    if (command.name == "timeline.stop") {
        std::string name = args.value("name", args.value("timeline", std::string()));
        if (!name.empty()) playedTimelines_.erase(name);
        return;
    }

}

bool ScenarioRunnerBehaviour::evaluateCondition(const json& condition) const {
    if (condition.is_null()) return true;
    if (!condition.is_object() || condition.size() != 1) return false;
    const std::string name = singleKey(condition);
    const json& args = condition.begin().value();

    if (name == "all") {
        if (!args.is_array()) return false;
        for (const auto& child : args)
            if (!evaluateCondition(child)) return false;
        return true;
    }
    if (name == "any") {
        if (!args.is_array()) return false;
        for (const auto& child : args)
            if (evaluateCondition(child)) return true;
        return false;
    }
    if (name == "not") return !evaluateCondition(args);
    if (name == "time.elapsed") {
        double seconds = args.is_number() ? args.get<double>() : args.value("seconds", 0.0);
        return stepElapsed_ >= seconds;
    }
    if (name == "blackboard.equals") {
        std::string key = args.value("key", std::string());
        auto it = blackboard_.find(key);
        json lhs = it != blackboard_.end() ? it->second : json();
        json rhs = args.contains("value") ? args["value"] : json();
        return jsonEquals(lhs, rhs);
    }
    if (name == "node.exists") return resolveNodeRef(args) != nullptr;
    if (name == "node.enabled") {
        Node* target = resolveNodeRef(args.contains("target") ? args["target"] : args);
        return target && target->enabled();
    }
    if (name == "group.count") {
        std::string group = args.value("group", std::string());
        int count = 0;
        if (node() && node()->tree()) count = static_cast<int>(node()->tree()->group(group).size());
        else if (Node* root = rootNode())
            root->traverse([&](Node& n, const glm::mat4&) { if (n.isInGroup(group)) ++count; });
        return compareCount(count, args.value("op", std::string("==")), args.value("value", 0));
    }
    if (name == "input.pressed") {
        std::string action = args.is_string() ? args.get<std::string>() : args.value("action", std::string());
        return !action.empty() && Input::isActionJustPressed(action);
    }
    if (name == "timeline.finished") {
        std::string timeline = args.is_string() ? args.get<std::string>() : args.value("timeline", args.value("name", std::string()));
        return !timeline.empty() && playedTimelines_.count(timeline) != 0;
    }
    if (name == "area.entered" || name == "area.exited" || name == "signal.received")
        return eventFacts_.count(conditionKey(condition)) != 0;
    return false;
}

void ScenarioRunnerBehaviour::subscribeCondition(const json& condition) {
    if (!condition.is_object() || condition.size() != 1) return;
    const std::string name = singleKey(condition);
    const json& args = condition.begin().value();
    if (name == "all" || name == "any") {
        if (args.is_array())
            for (const auto& child : args) subscribeCondition(child);
        return;
    }
    if (name == "not") {
        subscribeCondition(args);
        return;
    }
    if (name == "area.entered" || name == "area.exited") {
        Node* areaNode = resolveNodeRef(args.contains("area") ? args["area"] : args);
        auto* area = dynamic_cast<AreaNode*>(areaNode);
        if (!area) return;
        json captured = condition;
        std::string by = args.value("by", std::string());
        auto handler = [this, captured, by](CollisionObjectNode* body) {
            if (by.empty() || roleMatches(by, body)) markEvent(captured);
        };
        if (name == "area.entered") stepConnections_.push_back(area->bodyEntered.connect(handler));
        else stepConnections_.push_back(area->bodyExited.connect(handler));
        return;
    }
    if (name == "signal.received") {
        Node* target = resolveNodeRef(args.contains("target") ? args["target"] : args);
        std::string signal = args.value("signal", std::string());
        SignalHit hit = findSignal(target, signal);
        if (!hit.desc) return;
        json captured = condition;
        stepConnections_.push_back(hit.desc->connect(hit.obj, [this, captured](const json&) {
            markEvent(captured);
        }));
    }
}

Node* ScenarioRunnerBehaviour::resolveNodeRef(const json& ref) const {
    if (ref.is_string()) return resolveStringRef(ref.get<std::string>());
    if (!ref.is_object()) return nullptr;
    if (ref.contains("self") && ref["self"].get<bool>()) return node();
    if (ref.contains("role")) return resolveStringRef(ref["role"].get<std::string>());
    if (ref.contains("spawned")) return resolveStringRef(ref["spawned"].get<std::string>());
    if (ref.contains("anchor")) return findAnchor(ref["anchor"].get<std::string>());
    if (ref.contains("group")) return firstInGroup(ref["group"].get<std::string>());
    if (ref.contains("target")) return resolveNodeRef(ref["target"]);
    if (ref.contains("area")) return resolveNodeRef(ref["area"]);
    return nullptr;
}

Node* ScenarioRunnerBehaviour::resolveStringRef(const std::string& key) const {
    auto role = roles_.find(key);
    if (role != roles_.end()) return role->second;
    if (Node* anchor = findAnchor(key)) return anchor;
    if (Node* group = firstInGroup(key)) return group;
    return nullptr;
}

Node* ScenarioRunnerBehaviour::findAnchor(const std::string& key) const {
    Node* root = rootNode();
    if (!root) return nullptr;
    Node* found = nullptr;
    root->traverse([&](Node& n, const glm::mat4&) {
        if (found) return;
        if (auto* anchor = n.getBehaviour<ScenarioAnchor>())
            if (anchor->key == key) found = &n;
    });
    return found;
}

Node* ScenarioRunnerBehaviour::firstInGroup(const std::string& group) const {
    if (group.empty()) return nullptr;
    if (node() && node()->tree()) return node()->tree()->firstInGroup(group);
    Node* root = rootNode();
    if (!root) return nullptr;
    Node* found = nullptr;
    root->traverse([&](Node& n, const glm::mat4&) {
        if (!found && n.isInGroup(group)) found = &n;
    });
    return found;
}

Node* ScenarioRunnerBehaviour::rootNode() const {
    Node* n = node();
    if (!n) return nullptr;
    while (n->parent()) n = n->parent();
    return n;
}

void ScenarioRunnerBehaviour::resolveRoles() {
    roles_.clear();
    lastError_.clear();
    for (const auto& [name, role] : asset_.roles) {
        const json& r = role.data;
        Node* resolved = nullptr;
        if (r.is_object()) {
            if (r.contains("group")) resolved = firstInGroup(r["group"].get<std::string>());
            else if (r.contains("anchor")) resolved = findAnchor(r["anchor"].get<std::string>());
            else if (r.contains("self") && r["self"].get<bool>()) resolved = node();
            if (!resolved && r.contains("spawn") && node() && node()->tree()) {
                resolved = node()->tree()->instantiate(r["spawn"].get<std::string>());
                if (resolved) {
                    ownedNodes_.push_back(resolved);
                    if (r.contains("at"))
                        if (Node* at = resolveNodeRef(r["at"]))
                            resolved->transform().position = glm::vec3(at->worldTransform()[3]);
                }
            }
            if (!resolved && r.value("required", false)) {
                lastError_ = "required role '" + name + "' could not be resolved";
                return;
            }
        }
        if (resolved) roles_[name] = resolved;
    }
}

bool ScenarioRunnerBehaviour::roleMatches(const std::string& roleOrGroup, Node* candidate) const {
    if (!candidate) return false;
    auto role = roles_.find(roleOrGroup);
    if (role != roles_.end()) return role->second == candidate;
    return candidate->isInGroup(roleOrGroup);
}

std::string ScenarioRunnerBehaviour::conditionKey(const json& condition) const {
    return condition.dump();
}

void ScenarioRunnerBehaviour::markEvent(const json& condition) {
    eventFacts_.insert(conditionKey(condition));
}

void ScenarioRunnerBehaviour::cleanupOwned() {
    for (Node* owned : ownedNodes_)
        if (owned) owned->queueFree();
    ownedNodes_.clear();
}

void ScenarioRunnerBehaviour::describe(reflect::TypeBuilder<ScenarioRunnerBehaviour>& t) {
    t.doc("Runs a declarative .saidascenario asset. Use this for quests, tutorials, puzzles, waves, cinematics and missions.");
    t.property("scenarioPath", &ScenarioRunnerBehaviour::scenarioPath).asset().tooltip("project-relative .saidascenario file");
    t.property("autoStart", &ScenarioRunnerBehaviour::autoStart);
    t.property("cleanupOnStop", &ScenarioRunnerBehaviour::cleanupOnStop);
    t.signal("started", &ScenarioRunnerBehaviour::started);
    t.signal("stepChanged", &ScenarioRunnerBehaviour::stepChanged);
    t.signal("finished", &ScenarioRunnerBehaviour::finished);
    t.signal("failed", &ScenarioRunnerBehaviour::failed);
    t.slot("start", &ScenarioRunnerBehaviour::start);
    t.slot("stop", &ScenarioRunnerBehaviour::stop);
    t.slot("pause", &ScenarioRunnerBehaviour::pause);
    t.slot("resume", &ScenarioRunnerBehaviour::resume);
}

} // namespace saida
