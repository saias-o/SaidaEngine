#pragma once

#include "core/Paths.hpp"
#include "project/Project.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace saida::runtime {

// Applies an ephemeral autoload passed by a verification harness without
// rewriting the packaged .saidaproj. The value is NAME=project/relative.js.
// Normal runtime launches never use this option; when present it remains under
// the same canonical project root and JavaScript extension policy as imports.
inline bool applyTestAutoload(Project& project, const std::string& spec,
                              std::string& error) {
    const std::size_t separator = spec.find('=');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= spec.size()) {
        error = "test autoload must use NAME=project-relative-script";
        return false;
    }

    const std::string name = spec.substr(0, separator);
    if (!std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '_' || c == '-';
        })) {
        error = "test autoload name must contain only letters, digits, '_' or '-'";
        return false;
    }

    const auto resolved = resolveSandboxedProjectPath(
        project.rootPath(), spec.substr(separator + 1));
    if (!resolved) {
        error = "test autoload path: " + resolved.error;
        return false;
    }

    std::string extension = std::filesystem::path(resolved.relative).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".js" && extension != ".mjs") {
        error = "test autoload must reference a .js or .mjs file";
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(resolved.absolute, ec) || ec) {
        error = "test autoload script not found: " + resolved.relative;
        return false;
    }

    project.setAutoload(name, resolved.relative);
    return true;
}

} // namespace saida::runtime
