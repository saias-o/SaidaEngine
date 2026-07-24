#include "mcp/tools/ToolRegistry.hpp"

#include "mcp/tools/Tools.hpp"

#include <stdexcept>
#include <utility>

namespace saida::mcp {

void ToolRegistry::add(std::string name, std::string description,
                       json inputSchema, Handler handler) {
    if (!handler) throw std::runtime_error("MCP tool handler is empty");
    if (indices_.find(name) != indices_.end())
        throw std::runtime_error("duplicate MCP tool '" + name + "'");

    const size_t index = entries_.size();
    indices_.emplace(name, index);
    entries_.push_back(
        {std::move(name), std::move(description), std::move(inputSchema),
         std::move(handler)});
}

json ToolRegistry::list() const {
    json tools = json::array();
    for (const Entry& entry : entries_) {
        tools.push_back({{"name", entry.name},
                         {"description", entry.description},
                         {"inputSchema", entry.inputSchema}});
    }
    return tools;
}

json ToolRegistry::call(const ToolContext& context, const std::string& name,
                        const json& args) const {
    const auto found = indices_.find(name);
    if (found == indices_.end())
        throw std::runtime_error("unknown tool '" + name + "'");
    return entries_[found->second].handler(context, args);
}

json objectSchema(json properties, std::vector<std::string> required) {
    json schema{{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty()) schema["required"] = std::move(required);
    return schema;
}

json stringSchema() {
    return {{"type", "string"}};
}

void registerAllTools(ToolRegistry& registry) {
    registerIntrospectionTools(registry);
    registerSceneTools(registry);
    registerNodeTools(registry);
    registerAssetTools(registry);
    registerScenarioTools(registry);
    registerAnimationTools(registry);
}

} // namespace saida::mcp
