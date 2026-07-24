#include "mcp/tools/Tools.hpp"

#include "editor/Command.hpp"
#include "mcp/tools/ToolRegistry.hpp"
#include "scenario/ScenarioAsset.hpp"
#include "scenario/ScenarioRegistry.hpp"
#include "scenario/ScenarioRunnerBehaviour.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace saida::mcp {
namespace {
namespace fs = std::filesystem;

std::string scenarioAbsPath(const ToolContext& ctx, const json& args) {
    if (!args.contains("path")) fail("missing 'path'");
    return resolveToolPath(ctx, args["path"].get<std::string>()).absolute;
}

json scenarioIssuesJson(const std::vector<ScenarioIssue>& issues) {
    json out = json::array();
    for (const auto& issue : issues)
        out.push_back({{"path", issue.path}, {"message", issue.message}});
    return out;
}

json toolListScenarioActions(const ToolContext&, const json&) {
    return ScenarioActionRegistry::names();
}

json toolListScenarioConditions(const ToolContext&, const json&) {
    return ScenarioConditionRegistry::names();
}

json toolValidateScenario(const ToolContext& ctx, const json& args) {
    ScenarioAsset asset;
    std::vector<ScenarioIssue> issues;
    std::string abs = scenarioAbsPath(ctx, args);
    bool ok = ScenarioAsset::loadFromFile(abs, asset, &issues);
    return {{"ok", ok}, {"path", abs}, {"issues", scenarioIssuesJson(issues)}};
}

json toolGetScenario(const ToolContext& ctx, const json& args) {
    std::string abs = scenarioAbsPath(ctx, args);
    std::string text = readFile(abs);
    if (text.empty()) fail("could not read scenario: " + abs);
    return json::parse(text);
}

json toolCreateScenario(const ToolContext& ctx, const json& args) {
    if (!args.contains("path")) fail("missing 'path'");
    std::string abs = scenarioAbsPath(ctx, args);
    json doc = args.value("scenario", json::object());
    if (doc.empty()) {
        doc = {
            {"version", 1},
            {"id", fs::path(abs).stem().string()},
            {"roles", json::object()},
            {"blackboard", json::object()},
            {"steps", json::array({{{"id", "start"}, {"end", "success"}}})}
        };
    }
    ScenarioAsset asset;
    std::vector<ScenarioIssue> issues;
    ScenarioAsset::parse(doc, asset, &issues);
    if (!issues.empty()) return {{"ok", false}, {"path", abs}, {"issues", scenarioIssuesJson(issues)}};
    writeFile(abs, asset.toJson().dump(2) + "\n");
    return {{"ok", true}, {"path", abs}};
}

json toolUpdateScenarioStep(const ToolContext& ctx, const json& args) {
    if (!args.contains("step") || !args["step"].is_object()) fail("missing object 'step'");
    std::string abs = scenarioAbsPath(ctx, args);
    json doc = json::parse(readFile(abs));
    std::string id = args["step"].value("id", std::string());
    if (id.empty()) fail("step.id is required");
    json& steps = doc["steps"];
    if (!steps.is_array()) steps = json::array();
    bool replaced = false;
    for (auto& step : steps) {
        if (step.is_object() && step.value("id", std::string()) == id) {
            step = args["step"];
            replaced = true;
            break;
        }
    }
    if (!replaced) steps.push_back(args["step"]);

    ScenarioAsset asset;
    std::vector<ScenarioIssue> issues;
    ScenarioAsset::parse(doc, asset, &issues);
    if (!issues.empty()) return {{"ok", false}, {"path", abs}, {"issues", scenarioIssuesJson(issues)}};
    writeFile(abs, asset.toJson().dump(2) + "\n");
    return {{"ok", true}, {"path", abs}};
}

json toolAttachScenario(const ToolContext& ctx, const json& args) {
    requireEdit(ctx);
    Node* node = requireNode(ctx, args);
    if (!args.contains("path")) fail("missing 'path'");
    SandboxedPathResult scenarioPath = resolveToolPath(ctx, args["path"].get<std::string>());
    auto* runner = node->getBehaviour<ScenarioRunnerBehaviour>();
    if (!runner) {
        ctx.exec(std::make_unique<AddBehaviourCommand>(node->id(), "ScenarioRunner"));
        runner = node->getBehaviour<ScenarioRunnerBehaviour>();
    }
    if (!runner) fail("failed to attach ScenarioRunner");
    runner->scenarioPath = scenarioPath.relative;
    if (args.contains("autoStart")) runner->autoStart = args["autoStart"].get<bool>();
    return {{"ok", true},
            {"id", idString(node->id())},
            {"behaviour", "ScenarioRunner"},
            {"path", scenarioPath.absolute},
            {"relativePath", scenarioPath.relative}};
}

} // namespace

void registerScenarioTools(ToolRegistry& registry) {
    registry.add("list_scenario_actions",
                 "List the only valid declarative scenario action keys. LLMs must not invent actions.",
                 objectSchema({}), toolListScenarioActions);
    registry.add("list_scenario_conditions",
                 "List the only valid declarative scenario condition keys. LLMs must not invent conditions.",
                 objectSchema({}), toolListScenarioConditions);
    registry.add("create_scenario",
                 "Create a .saidascenario JSON asset. {path, scenario?}. Validates before writing.",
                 objectSchema({{"path", stringSchema()},
                               {"scenario", json{{"type", "object"}}}}, {"path"}),
                 toolCreateScenario);
    registry.add("validate_scenario",
                 "Validate a .saidascenario asset with strict action/condition/schema checks.",
                 objectSchema({{"path", stringSchema()}}, {"path"}),
                 toolValidateScenario);
    registry.add("get_scenario", "Read a .saidascenario asset as JSON.",
                 objectSchema({{"path", stringSchema()}}, {"path"}),
                 toolGetScenario);
    registry.add("update_scenario_step",
                 "Replace or append one scenario step in a .saidascenario, validating before writing.",
                 objectSchema({{"path", stringSchema()},
                               {"step", json{{"type", "object"}}}},
                              {"path", "step"}), toolUpdateScenarioStep);
    registry.add("attach_scenario",
                 "Attach a ScenarioRunner to a node and point it at a .saidascenario.",
                 objectSchema({{"id", stringSchema()}, {"path", stringSchema()},
                               {"autoStart", json{{"type", "boolean"}}}},
                              {"id", "path"}), toolAttachScenario);
}

} // namespace saida::mcp
