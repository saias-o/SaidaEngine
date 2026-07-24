#include "mcp/tools/ToolRegistry.hpp"

#include <cassert>
#include <set>
#include <stdexcept>
#include <string>

int main() {
    saida::mcp::ToolRegistry registry;
    saida::mcp::registerAllTools(registry);

    const saida::mcp::json catalog = registry.list();
    assert(catalog.is_array());
    assert(catalog.size() == 45);

    std::set<std::string> names;
    for (const auto& tool : catalog) {
        assert(tool.contains("name") && tool["name"].is_string());
        assert(tool.contains("description") && tool["description"].is_string());
        assert(tool.contains("inputSchema") &&
               tool["inputSchema"].is_object());
        assert(tool["inputSchema"].value("type", std::string()) == "object");
        names.insert(tool["name"].get<std::string>());
    }
    assert(names.size() == catalog.size());

    const std::set<std::string> expected{
        "add_animation_state", "add_animation_transition", "add_behaviour",
        "add_to_group", "attach_scenario", "authoring_guide", "build",
        "configure_behaviour", "connect_signal", "create_animation_graph",
        "create_animation_sequence", "create_clip_view",
        "create_locomotion_graph", "create_node", "create_scenario",
        "delete_node", "describe_api", "find_nodes", "get_node",
        "get_scenario", "get_scene", "import_model",
        "inspect_animation_asset", "list_animation_assets",
        "list_behaviour_types", "list_node_types", "list_recipes",
        "list_scenario_actions", "list_scenario_conditions",
        "preview_animation_asset", "read_logs", "remove_from_group",
        "rename_node", "reparent_node", "run_headless_check",
        "set_graph_parameter", "set_property", "set_scene_settings",
        "set_transform", "update_scenario_step",
        "validate_animation_asset", "validate_scenario",
        "write_cpp_behaviour", "write_script", "write_ui"};
    assert(names == expected);

    bool duplicateRejected = false;
    try {
        registry.add("describe_api", "duplicate", saida::mcp::objectSchema({}),
                     [](const saida::mcp::ToolContext&,
                        const saida::mcp::json&) {
                         return saida::mcp::json::object();
                     });
    } catch (const std::runtime_error&) {
        duplicateRejected = true;
    }
    assert(duplicateRejected);

    bool unknownRejected = false;
    try {
        registry.call({}, "missing_tool", saida::mcp::json::object());
    } catch (const std::runtime_error&) {
        unknownRejected = true;
    }
    assert(unknownRejected);
    return 0;
}
