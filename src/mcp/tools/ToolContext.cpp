#include "mcp/tools/ToolContext.hpp"

#include "core/Paths.hpp"
#include "editor/SceneDocument.hpp"
#include "project/Project.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace saida::mcp {

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void requireEdit(const ToolContext& context) {
    if (!context.canEdit) fail("scene is read-only while in Play mode");
}

NodeId parseNodeId(const json& value) {
    if (value.is_string()) {
        try {
            return std::stoull(value.get<std::string>());
        } catch (...) {
            return kNodeInvalid;
        }
    }
    if (value.is_number_unsigned()) return value.get<uint64_t>();
    if (value.is_number_integer())
        return static_cast<NodeId>(value.get<int64_t>());
    return kNodeInvalid;
}

std::string idString(NodeId id) {
    // NodeId is 64-bit; strings preserve precision for JavaScript clients.
    return std::to_string(id);
}

Node* requireNode(const ToolContext& context, const json& args,
                  const char* key) {
    if (!args.contains(key)) fail(std::string("missing '") + key + "'");
    const NodeId id = parseNodeId(args[key]);
    Node* node = context.doc ? context.doc->find(id) : nullptr;
    if (!node) fail("no node with id " + idString(id));
    return node;
}

glm::vec3 vec3From(const json& value, const glm::vec3& fallback) {
    if (value.is_array() && value.size() == 3) {
        return {value[0].get<float>(), value[1].get<float>(),
                value[2].get<float>()};
    }
    return fallback;
}

std::string readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    std::stringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

void writeFile(const std::string& path, const std::string& content) {
    const std::filesystem::path destination(path);
    std::error_code error;
    if (destination.has_parent_path())
        std::filesystem::create_directories(destination.parent_path(), error);
    std::ofstream output(path, std::ios::binary);
    if (!output) fail("could not write " + path);
    output << content;
}

std::string sanitizeIdentifier(const std::string& value) {
    std::string result;
    for (char character : value) {
        if (std::isalnum(static_cast<unsigned char>(character)) ||
            character == '_') {
            result += character;
        }
    }
    if (result.empty() ||
        std::isdigit(static_cast<unsigned char>(result.front()))) {
        result = "B" + result;
    }
    return result;
}

std::string projectRoot(const ToolContext& context) {
    if (context.project && context.project->isLoaded())
        return context.project->rootPath();
    return std::string(SAIDA_PROJECT_ROOT);
}

SandboxedPathResult resolveToolPath(const ToolContext& context,
                                    const std::string& path,
                                    const std::string& defaultDirectory) {
    SandboxedPathResult resolved =
        resolveSandboxedProjectPath(projectRoot(context), path, defaultDirectory);
    if (!resolved) fail(resolved.error + ": " + path);
    return resolved;
}

} // namespace saida::mcp
