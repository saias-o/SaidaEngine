#pragma once

// Immutable-after-construction MCP tool catalog. Each descriptor and handler is
// registered together, so tools/list and tools/call cannot drift apart.

#include "mcp/tools/ToolContext.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace saida::mcp {

class ToolRegistry {
public:
    using Handler =
        std::function<json(const ToolContext&, const json&)>;

    void add(std::string name, std::string description, json inputSchema,
             Handler handler);
    json list() const;
    json call(const ToolContext& context, const std::string& name,
              const json& args) const;

private:
    struct Entry {
        std::string name;
        std::string description;
        json inputSchema;
        Handler handler;
    };

    std::vector<Entry> entries_;
    std::unordered_map<std::string, size_t> indices_;
};

json objectSchema(json properties,
                  std::vector<std::string> required = {});
json stringSchema();

void registerAllTools(ToolRegistry& registry);

} // namespace saida::mcp
