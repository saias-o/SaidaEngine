#pragma once
#include <string>

namespace saida::mcp {
struct ProjectBuildResult { int exitCode = -1; std::string output; };
ProjectBuildResult buildNativeProject(const std::string& directory);
} // namespace saida::mcp
