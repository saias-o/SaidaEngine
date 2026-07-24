#pragma once

// Shared editor state for MCP tools. Mutations must pass through `exec`, which
// preserves the editor's Play-mode lock and undo/redo invariant.

#include "core/Paths.hpp"
#include "editor/Command.hpp"
#include "scene/Node.hpp"

#include "nlohmann/json.hpp"

#include <functional>
#include <memory>
#include <string>

namespace saida {

class Project;
class ResourceManager;
class Scene;
class SceneDocument;

namespace mcp {

using json = nlohmann::json;

struct ToolContext {
    Scene* scene = nullptr;
    ResourceManager* resources = nullptr;
    SceneDocument* doc = nullptr;
    Project* project = nullptr;
    bool canEdit = false;
    std::function<void(std::unique_ptr<Command>)> exec;
};

[[noreturn]] void fail(const std::string& message);
void requireEdit(const ToolContext& context);

NodeId parseNodeId(const json& value);
std::string idString(NodeId id);
Node* requireNode(const ToolContext& context, const json& args,
                  const char* key = "id");
glm::vec3 vec3From(const json& value, const glm::vec3& fallback);

std::string readFile(const std::string& path);
void writeFile(const std::string& path, const std::string& content);
std::string sanitizeIdentifier(const std::string& value);
std::string projectRoot(const ToolContext& context);
SandboxedPathResult resolveToolPath(const ToolContext& context,
                                    const std::string& path,
                                    const std::string& defaultDirectory = {});

} // namespace mcp
} // namespace saida
