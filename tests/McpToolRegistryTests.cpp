#include "mcp/tools/ToolRegistry.hpp"
#include "project/Project.hpp"
#include "core/Paths.hpp"
#include "editor/BuildExporter.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
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

    namespace fs = std::filesystem;
    using json = saida::mcp::json;
    const auto root = fs::temp_directory_path() / ("saida-native-project-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{root};
    auto read = [](const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream text; text << file.rdbuf(); return text.str();
    };
    const auto engineRegistry = fs::path(saida::engineRoot()) / "src/scene/ReflectedTypes.cpp";
    const auto registryBefore = read(engineRegistry);
    saida::Project project;
    assert(project.create(root.string(), "A game & tools"));
    saida::mcp::ToolContext context;
    context.project = &project; context.canEdit = true;
    auto rejects = [&](const std::string& name, const json& args) {
        bool rejected = false;
        try { registry.call(context, name, args); } catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    };
    auto output = registry.call(context, "write_cpp_behaviour", {
        {"name", "ProjectSpin"}, {"doc", "A \"quoted\" doc\nwith a second line"},
        {"properties", json::array({{{"name", "label"}, {"type", "string"}, {"default", "path\\file\n\"quoted\""}},
            {{"name", "speed"}, {"type", "float"}, {"default", 1e-9}}})}});
    const auto header = root / "A game & tools/src/generated/ProjectSpin.hpp";
    const auto source = root / "A game & tools/src/generated/ProjectSpin.cpp";
    assert(fs::is_regular_file(header) && fs::is_regular_file(source));
    assert(read(header).find("namespace game") != std::string::npos);
    assert(read(header).find("\\012") != std::string::npos);
    assert(read(source).find("generated/ProjectSpin") == std::string::npos);
    const auto firstHeader = read(header);
    saida::BuildExporter::Options exportOptions;
    exportOptions.outputDir = (root / "export").string();
    auto nativeExport = saida::BuildExporter::exportWindowsBuild(project, exportOptions);
    assert(!nativeExport.success && nativeExport.error.find("--runtime") != std::string::npos);
    auto webExport = saida::BuildExporter::exportWebBuild(project, exportOptions);
    assert(!webExport.success && webExport.error.find("project-native C++") != std::string::npos);
    assert(!fs::exists(root / "export"));
    rejects("write_cpp_behaviour", {{"name", "ProjectSpin"}});
    assert(read(header) == firstHeader);
    rejects("write_cpp_behaviour", {{"name", "../../escape"}});
    rejects("write_cpp_behaviour", {{"name", "class"}});
    rejects("write_cpp_behaviour", {{"name", "BadType"}, {"properties", json::array({{{"name", "value"}, {"type", "matrix"}}})}});
    rejects("write_cpp_behaviour", {{"name", "BadDefault"}, {"properties", json::array({{{"name", "value"}, {"type", "int"}, {"default", 1.5}}})}});
    rejects("build", {{"directory", "../outside"}});
    rejects("build", json::object()); // No configured native build yet.
    context.canEdit = false;
    rejects("write_cpp_behaviour", {{"name", "DuringPlay"}});
    rejects("build", json::object());
    context.canEdit = true; context.project = nullptr;
    rejects("write_cpp_behaviour", {{"name", "NoProject"}});
    assert(read(engineRegistry) == registryBefore);
    if (argc == 2) {
        saida::Project external;
        assert(external.load(argv[1]));
        context.project = &external;
        if (!fs::exists(fs::path(external.rootPath()) / "src/generated/SdkGenerated.cpp"))
            registry.call(context, "write_cpp_behaviour", {
                {"name", "SdkGenerated"}, {"doc", "Quoted \"documentation\"\nSecond line"},
                {"properties", json::array({{{"name", "text"}, {"type", "string"},
                    {"default", "a\\path\nwith \"quotes\""}},
                    {{"name", "speed"}, {"type", "float"}, {"default", 1e-9}}})}});
        assert(registry.call(context, "build", json::object())["ok"] == true);
    }
    return 0;
}
