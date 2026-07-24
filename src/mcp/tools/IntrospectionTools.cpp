#include "mcp/tools/Tools.hpp"

#include "core/Reflection.hpp"
#include "mcp/tools/ToolRegistry.hpp"
#include "scene/BehaviourRegistry.hpp"
#include "scene/NodeRegistry.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace saida::mcp {
namespace {

std::vector<std::string> sortedKeys(const std::unordered_map<std::string, std::function<std::unique_ptr<Node>()>>& m) {
    std::vector<std::string> keys;
    keys.reserve(m.size());
    for (const auto& [k, _] : m) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    return keys;
}
std::vector<std::string> sortedKeys(const std::unordered_map<std::string, std::function<std::unique_ptr<Behaviour>()>>& m) {
    std::vector<std::string> keys;
    keys.reserve(m.size());
    for (const auto& [k, _] : m) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    return keys;
}

std::string manifestHash() {
    std::string dump = reflect::TypeRegistry::instance().manifest("").dump();
    return std::to_string(std::hash<std::string>{}(dump));
}

json toolDescribeApi(const ToolContext&, const json& args) {
    auto& reg = reflect::TypeRegistry::instance();
    if (args.contains("type")) return reg.manifestFor(args["type"].get<std::string>());

    // Summary mode avoids rebuilding an unchanged full manifest.
    if (args.value("summary", false)) {
        json out, behaviours = json::array(), nodes = json::array();
        json all = reg.manifest("");
        for (const auto& t : all.value("behaviours", json::array())) behaviours.push_back(t["name"]);
        for (const auto& t : all.value("nodes", json::array())) nodes.push_back(t["name"]);
        out["behaviours"] = std::move(behaviours);
        out["nodes"] = std::move(nodes);
        out["hash"] = manifestHash();
        return out;
    }

    json full = reg.manifest(args.value("category", std::string()));
    full["hash"] = manifestHash();  // cache key: unchanged hash => reuse last manifest
    return full;
}

json toolListNodeTypes(const ToolContext&, const json&) {
    return sortedKeys(NodeRegistry::instance().factories());
}
json toolListBehaviourTypes(const ToolContext&, const json&) {
    return sortedKeys(BehaviourRegistry::instance().factories());
}

json toolAuthoringGuide(const ToolContext&, const json&) {
    static const char* guide =
        "SaidaEngine authoring. One paradigm: nodes + behaviours + signals.\n"
        "RULES: 1) all logic is a Behaviour (no manager classes). 2) compose small\n"
        "focused behaviours, don't grow god-classes. 3) call down, signal up. 4) no\n"
        "globals except services/autoloads (Blackboard). 5) find nodes by group or\n"
        "scoped query, never by global name.\n\n"
        "WORKFLOW:\n"
        "- describe_api {summary:true} to list types; then {type:'X'} for one type's\n"
        "  properties/signals/slots. Cache by 'hash'.\n"
        "- Build scenes with create_node / set_property / set_transform / add_to_group.\n"
        "- Behaviour: add_behaviour, then set_property (scalars) or configure_behaviour\n"
        "  (full JSON: StateMachine/Blackboard/script props).\n"
        "- Wire events: connect_signal {from,signal,to,slot} (reflected names).\n"
        "- Scenario flows: use create_scenario / update_scenario_step /\n"
        "  validate_scenario / attach_scenario. Do not write ScriptBehaviour code\n"
        "  to orchestrate tutorials, quests, puzzles, waves, cinematics, boss phases\n"
        "  or missions; scripts are only for reusable local capabilities.\n"
        "- Code: write_script (JS, fast iteration) or write_cpp_behaviour + build (perf).\n"
        "  UI: write_ui (HTML/RML/CSS).\n"
        "- Validate: run_headless_check (scripts), read_logs. Maps: import_model.\n"
        "- Complex behaviour: see list_recipes (NPC, trigger light, scripted sequence).\n"
        "All edits are undoable and rejected during Play.";
    return {{"guide", guide}};
}

json toolListRecipes(const ToolContext&, const json&) {
    return json::array({
        {{"name", "npc_patrol_chase"},
         {"summary", "An NPC that patrols, sees the player via a trigger volume, and chases."},
         {"steps", json::array({
             "create_node CharacterBody 'NPC'",
             "add_behaviour Character on the NPC (movement)",
             "create_node Area 'Vision' as a child (perception volume) + a CollisionShape child",
             "add_behaviour StateMachine on the NPC",
             "configure_behaviour StateMachine {initialState:'patrol', states:['patrol','chase'], transitions:[{from:'patrol',to:'chase',when:{key:'sawPlayer',op:'==',value:1}},{from:'chase',to:'patrol',after:3}]}",
             "create a Blackboard node, add it to group 'blackboard'",
             "connect_signal Vision.bodyEntered -> (a script slot or set blackboard sawPlayer=1)"})}},
        {{"name", "trigger_light"},
         {"summary", "A light that turns on when the player enters an area."},
         {"steps", json::array({
             "create_node LightNode 'Lamp' (set intensity 0 via set_property)",
             "create_node Area 'Switch' + CollisionShape child",
             "write_script a small JS that listens and sets the light intensity, attachTo the Area",
             "or connect_signal Switch.bodyEntered -> a behaviour slot that enables the light"})}},
        {{"name", "scripted_sequence"},
         {"summary", "A generic declarative scenario sequence; prefer this over custom script orchestration."},
         {"steps", json::array({
             "create_scenario {path:'scenarios/intro.saidascenario', scenario:{version:1,id:'intro',roles:{player:{group:'player',required:true}},blackboard:{},steps:[{id:'start',enter:[{'objective.show':{text:'Reach the marker'}}],wait:{'area.entered':{area:'marker',by:'player'}},next:'done'},{id:'done',enter:[{'objective.complete':{}}],end:'success'}]}}",
             "attach_scenario the created .saidascenario to a node with ScenarioRunner",
             "validate_scenario before saving or continuing"})}},
        {{"name", "enemy_wave"},
         {"summary", "Spawn a wave and complete when the enemy group is empty."},
         {"steps", json::array({
             "place a ScenarioAnchor with key 'wave_spawn'",
             "create_scenario with scene.instantiate actions for enemy prefabs at 'wave_spawn'",
             "wait on {'group.count':{group:'enemy',op:'==',value:0}}",
             "end success"})}},
        {{"name", "boss_phases"},
         {"summary", "Drive boss phases with blackboard and transitions."},
         {"steps", json::array({
             "boss local behaviours expose health/combat capabilities",
             "scenario transitions watch blackboard.equals or signal.received",
             "enter actions call slots or emit signals to switch phase behaviours",
             "never put the phase graph inside the boss onUpdate"})}}
    });
}


} // namespace

void registerIntrospectionTools(ToolRegistry& registry) {
    registry.add("authoring_guide", "Authoring contract and tool workflow.",
                 objectSchema({}), toolAuthoringGuide);
    registry.add("describe_api",
        "Reflected type manifest. {summary:true} lists names only (cheap); {type:'X'} one type; {category:'behaviour'|'node'} filters. Cache by returned 'hash'.",
        objectSchema({{"summary", json{{"type", "boolean"}}},
                      {"category", stringSchema()}, {"type", stringSchema()}}),
        toolDescribeApi);
    registry.add("list_node_types", "Names of all instantiable node types.",
                 objectSchema({}), toolListNodeTypes);
    registry.add("list_behaviour_types",
                 "Names of all attachable behaviour types.", objectSchema({}),
                 toolListBehaviourTypes);
    registry.add("list_recipes",
                 "Curated composition recipes for complex things (NPC, trigger light, scripted sequence).",
                 objectSchema({}), toolListRecipes);
}

} // namespace saida::mcp
