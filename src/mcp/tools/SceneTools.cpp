#include "mcp/tools/Tools.hpp"

#include "editor/Command.hpp"
#include "editor/SceneDocument.hpp"
#include "graphics/ResourceManager.hpp"
#include "mcp/tools/ToolRegistry.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/SignalWiring.hpp"

#include <string>

namespace saida::mcp {
namespace {

json compactNode(const Node& node) {
    json j;
    j["id"] = idString(node.id());
    j["name"] = node.name();
    j["type"] = node.typeName();
    if (!node.enabled()) j["enabled"] = false;
    if (!node.groups().empty()) j["groups"] = node.groups();

    json behs = json::array();
    for (const auto& b : node.behaviours())
        if (b->typeName()) behs.push_back(b->typeName());
    if (!behs.empty()) j["behaviours"] = std::move(behs);

    json children = json::array();
    for (const auto& c : node.children()) children.push_back(compactNode(*c));
    if (!children.empty()) j["children"] = std::move(children);
    return j;
}

json toolGetScene(const ToolContext& ctx, const json&) {
    if (!ctx.scene) fail("no scene bound");
    json out = compactNode(*ctx.scene);
    if (!ctx.scene->connections().empty()) {
        json conns = json::array();
        for (const auto& c : ctx.scene->connections())
            conns.push_back({{"from", idString(c.from)}, {"signal", c.signal},
                             {"to", idString(c.to)}, {"slot", c.slot}});
        out["connections"] = std::move(conns);
    }
    return out;
}

json toolGetNode(const ToolContext& ctx, const json& args) {
    Node* node = requireNode(ctx, args);
    if (!ctx.resources) fail("no resources bound");
    return json::parse(SceneSerializer::nodeToJson(*node, *ctx.resources));
}

json toolFindNodes(const ToolContext& ctx, const json& args) {
    if (!ctx.scene) fail("no scene bound");
    std::string group = args.value("group", std::string());
    std::string type = args.value("type", std::string());
    json hits = json::array();
    ctx.scene->traverse([&](Node& n, const glm::mat4&) {
        if (!group.empty() && !n.isInGroup(group)) return;
        if (!type.empty() && type != n.typeName()) return;
        hits.push_back({{"id", idString(n.id())}, {"name", n.name()}, {"type", n.typeName()}});
    });
    return hits;
}

json toolConnectSignal(const ToolContext& ctx, const json& args) {
    requireEdit(ctx);
    for (const char* k : {"from", "signal", "to", "slot"})
        if (!args.contains(k)) fail(std::string("missing '") + k + "'");
    SignalConnectionDef def;
    def.from = parseNodeId(args["from"]);
    def.signal = args["signal"].get<std::string>();
    def.to = parseNodeId(args["to"]);
    def.slot = args["slot"].get<std::string>();
    if (!ctx.doc->find(def.from)) fail("no 'from' node with id " + idString(def.from));
    if (!ctx.doc->find(def.to)) fail("no 'to' node with id " + idString(def.to));
    ctx.exec(std::make_unique<ConnectSignalCommand>(std::move(def)));
    return {{"ok", true}};
}

json toolSetSceneSettings(const ToolContext& ctx, const json& args) {
    requireEdit(ctx);
    if (!ctx.scene) fail("no scene bound");
    SceneSettings oldS = ctx.scene->settings();
    SceneSettings newS = oldS;
    if (args.contains("ambient")) newS.ambientLight = glm::vec4(vec3From(args["ambient"], glm::vec3(oldS.ambientLight)), 0.0f);
    if (args.contains("clearColor")) newS.clearColor = glm::vec4(vec3From(args["clearColor"], glm::vec3(oldS.clearColor)), 1.0f);
    if (args.contains("giEnabled")) newS.giEnabled = args["giEnabled"].get<bool>();
    if (args.contains("giIntensity")) newS.giIntensity = args["giIntensity"].get<float>();
    if (args.contains("fogEnabled")) newS.fogEnabled = args["fogEnabled"].get<bool>();
    if (args.contains("fogColor")) newS.fogColor = glm::vec4(vec3From(args["fogColor"], glm::vec3(oldS.fogColor)), 1.0f);
    if (args.contains("fogDensity")) newS.fogDensity = args["fogDensity"].get<float>();
    if (args.contains("bloomEnabled")) newS.bloomEnabled = args["bloomEnabled"].get<bool>();
    if (args.contains("bloomIntensity")) newS.bloomIntensity = args["bloomIntensity"].get<float>();
    if (args.contains("aoEnabled")) newS.aoEnabled = args["aoEnabled"].get<bool>();
    if (args.contains("skyboxExposure")) newS.skyboxExposure = args["skyboxExposure"].get<float>();

    NodeId rootId = ctx.scene->id();
    ctx.exec(std::make_unique<SetPropertyCommand>(
        rootId, "Scene Settings",
        [oldS](Node& n) { if (auto* s = dynamic_cast<Scene*>(&n)) s->settings() = oldS; },
        [newS](Node& n) { if (auto* s = dynamic_cast<Scene*>(&n)) s->settings() = newS; }));
    return {{"ok", true}};
}

} // namespace

void registerSceneTools(ToolRegistry& registry) {
    registry.add("get_scene",
                 "Compact tree of the current scene (ids, names, types, behaviours, connections).",
                 objectSchema({}), toolGetScene);
    registry.add("get_node", "Full JSON of one node (all serialized fields).",
                 objectSchema({{"id", stringSchema()}}, {"id"}), toolGetNode);
    registry.add("find_nodes",
                 "Find nodes by {group} and/or {type}; returns id/name/type.",
                 objectSchema({{"group", stringSchema()}, {"type", stringSchema()}}),
                 toolFindNodes);
    registry.add("connect_signal",
                 "Wire a reflected signal on {from} to a reflected slot on {to}.",
                 objectSchema({{"from", stringSchema()}, {"signal", stringSchema()},
                               {"to", stringSchema()}, {"slot", stringSchema()}},
                              {"from", "signal", "to", "slot"}),
                 toolConnectSignal);
    registry.add("set_scene_settings",
                 "Set scene-wide rendering (ambient, clearColor, gi*, fog*, bloom*, aoEnabled, skyboxExposure).",
                 objectSchema({}), toolSetSceneSettings);
}

} // namespace saida::mcp
