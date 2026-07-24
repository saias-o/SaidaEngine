#include "mcp/McpBridge.hpp"

#include "editor/EditorUI.hpp"
#include "mcp/McpServer.hpp"
#include "mcp/tools/ToolContext.hpp"
#include "mcp/tools/ToolRegistry.hpp"

#include "nlohmann/json.hpp"

#include <stdexcept>
#include <utility>

namespace saida {

using json = nlohmann::json;

McpBridge::McpBridge()
    : server_(std::make_unique<McpServer>()),
      tools_(std::make_unique<mcp::ToolRegistry>()) {
    mcp::registerAllTools(*tools_);
}

McpBridge::~McpBridge() = default;

bool McpBridge::start(uint16_t port) {
    return server_->start(port);
}

bool McpBridge::running() const {
    return server_ && server_->running();
}

void McpBridge::poll(EditorUI& ui) {
    if (!server_) return;
    server_->poll([this, &ui](const std::string& method, const json& params) {
        return dispatch(ui, method, params);
    });
}

json McpBridge::dispatch(EditorUI& ui, const std::string& method,
                         const json& params) {
    if (method == "initialize") {
        return {{"protocolVersion", "2024-11-05"},
                {"capabilities", {{"tools", json::object()}}},
                {"serverInfo", {{"name", "SaidaEngine"}, {"version", "0.1"}}}};
    }
    if (method == "notifications/initialized" || method == "ping")
        return json::object();
    if (method == "tools/list") return {{"tools", tools_->list()}};
    if (method == "tools/call") {
        const std::string name = params.value("name", std::string());
        const json args =
            params.contains("arguments") ? params["arguments"] : json::object();
        const json result = callTool(ui, name, args);
        // MCP wraps tool output as content items; expose the JSON as text.
        return {{"content", json::array(
                                {{{"type", "text"},
                                  {"text", result.dump(2)}}})}};
    }
    throw std::runtime_error("unknown method '" + method + "'");
}

json McpBridge::callTool(EditorUI& ui, const std::string& name,
                         const json& args) {
    mcp::ToolContext context;
    context.doc = &ui.document_;
    context.scene = ui.document_.scene();
    context.resources = ui.ctxResources_;
    context.project = ui.ctxProject_;
    context.canEdit = ui.canEdit();
    context.exec = [&ui](std::unique_ptr<Command> command) {
        ui.execute(std::move(command));
    };
    return tools_->call(context, name, args);
}

} // namespace saida
