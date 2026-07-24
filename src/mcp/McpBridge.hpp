#pragma once

// MCP edits use the editor command history so validation and undo share one path.

#include "nlohmann/json_fwd.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace saida {

class EditorUI;
class McpServer;
namespace mcp {
class ToolRegistry;
}

class McpBridge {
public:
    McpBridge();
    ~McpBridge();
    McpBridge(const McpBridge&) = delete;
    McpBridge& operator=(const McpBridge&) = delete;

    bool start(uint16_t port);
    bool running() const;

    // Drain + dispatch any queued MCP requests on the main thread. The editor
    // calls this once per frame while `ui` is fully bound (ctx pointers set).
    void poll(EditorUI& ui);

private:
    nlohmann::json dispatch(EditorUI& ui, const std::string& method, const nlohmann::json& params);
    nlohmann::json callTool(EditorUI& ui, const std::string& name, const nlohmann::json& args);

    std::unique_ptr<McpServer> server_;
    std::unique_ptr<mcp::ToolRegistry> tools_;
};

} // namespace saida
