#pragma once

// Domain registration points. Tool implementations stay private to their
// module; only coherent catalog population crosses translation-unit boundaries.

namespace saida::mcp {

class ToolRegistry;

void registerIntrospectionTools(ToolRegistry& registry);
void registerSceneTools(ToolRegistry& registry);
void registerNodeTools(ToolRegistry& registry);
void registerAssetTools(ToolRegistry& registry);
void registerScenarioTools(ToolRegistry& registry);
void registerAnimationTools(ToolRegistry& registry);

} // namespace saida::mcp
