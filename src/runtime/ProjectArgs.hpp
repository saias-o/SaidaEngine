#pragma once

#include <string>

namespace saida::runtime {

inline bool parseProjectArgs(int argc, char** argv, std::string& project, std::string& error) {
    bool seen = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) != "--project") continue;
        if (seen || i + 1 == argc || !*argv[i + 1] || std::string(argv[i + 1]).rfind("--", 0) == 0) {
            error = "--project requires one project file or directory and may appear only once";
            return false;
        }
        seen = true;
        project = argv[++i];
    }
    return true;
}

} // namespace saida::runtime
