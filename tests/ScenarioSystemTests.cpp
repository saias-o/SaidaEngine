#include "scene/ReflectedTypes.hpp"
#include "behaviours/RotatorBehaviour.hpp"
#include "scene/Scene.hpp"
#include "scenario/ScenarioAsset.hpp"
#include "scenario/ScenarioRunnerBehaviour.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;

namespace {

std::string writeScenario(const json& doc, const std::string& name) {
    std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path);
    file << doc.dump(2) << "\n";
    return path.string();
}

json baseScenario() {
    return {
        {"schema", 1},
        {"version", 1},
        {"id", "test.scenario"},
        {"roles", json::object()},
        {"blackboard", json::object()},
        {"steps", json::array({{{"id", "start"}, {"end", "success"}}})}
    };
}

void testValidation() {
    saida::ScenarioAsset asset;
    std::vector<saida::ScenarioIssue> issues;

    json dup = baseScenario();
    dup["steps"] = json::array({{{"id", "a"}}, {{"id", "a"}}});
    assert(!saida::ScenarioAsset::parse(dup, asset, &issues));
    assert(!issues.empty());

    json unknown = baseScenario();
    unknown["steps"] = json::array({
        {{"id", "a"}, {"enter", json::array({{{"invented.action", json::object()}}})}, {"end", "success"}}
    });
    issues.clear();
    assert(!saida::ScenarioAsset::parse(unknown, asset, &issues));
    assert(!issues.empty());

    json ok = baseScenario();
    issues.clear();
    assert(saida::ScenarioAsset::parse(ok, asset, &issues));
    assert(issues.empty());
    assert(asset.toJson()["id"] == "test.scenario");
}

void testTimedRuntime() {
    json doc = baseScenario();
    doc["blackboard"] = {{"ready", false}};
    doc["steps"] = json::array({
        {{"id", "start"},
         {"enter", json::array({{{"blackboard.set", {{"key", "ready"}, {"value", true}}}}})},
         {"wait", {{"time.elapsed", {{"seconds", 0.25}}}}},
         {"next", "done"}},
        {{"id", "done"}, {"end", "success"}}
    });
    std::string path = writeScenario(doc, "saida_scenario_timed.saidascenario");

    saida::Scene scene;
    auto* runnerNode = scene.createChild<saida::Node>("Runner");
    auto* runner = runnerNode->addBehaviour<saida::ScenarioRunnerBehaviour>();
    runner->scenarioPath = path;
    runner->autoStart = false;

    std::string result;
    auto conn = runner->finished.connect([&](std::string r) { result = r; });
    runner->start();
    assert(runner->running());
    assert(runner->currentStep() == "start");
    runner->onUpdate(0.1f);
    assert(result.empty());
    runner->onUpdate(0.2f);
    assert(result == "success");
}

void testSignalRuntime() {
    json doc = baseScenario();
    doc["roles"] = {{"emitter", {{"group", "emitter"}, {"required", true}}}};
    doc["steps"] = json::array({
        {{"id", "wait_signal"},
         {"wait", {{"signal.received", {{"target", "emitter"}, {"signal", "fullRotation"}}}}},
         {"next", "done"}},
        {{"id", "done"}, {"end", "success"}}
    });
    std::string path = writeScenario(doc, "saida_scenario_signal.saidascenario");

    saida::Scene scene;
    auto* emitter = scene.createChild<saida::Node>("Emitter");
    emitter->addToGroup("emitter");
    auto* rotator = emitter->addBehaviour<saida::RotatorBehaviour>();
    auto* runnerNode = scene.createChild<saida::Node>("Runner");
    auto* runner = runnerNode->addBehaviour<saida::ScenarioRunnerBehaviour>();
    runner->scenarioPath = path;
    runner->autoStart = false;

    std::string result;
    auto conn = runner->finished.connect([&](std::string r) { result = r; });
    runner->start();
    assert(runner->running());
    rotator->fullRotation.emit();
    runner->onUpdate(0.01f);
    assert(result == "success");
}

} // namespace

int main() {
    saida::registerReflectedTypes();
    testValidation();
    testTimedRuntime();
    testSignalRuntime();
    return 0;
}
