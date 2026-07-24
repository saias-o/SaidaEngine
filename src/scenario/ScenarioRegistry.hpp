#pragma once

#include <string>
#include <vector>

namespace saida {

class ScenarioActionRegistry {
public:
    static bool isKnown(const std::string& name);
    static std::vector<std::string> names();
};

class ScenarioConditionRegistry {
public:
    static bool isKnown(const std::string& name);
    static bool isComposite(const std::string& name);
    static std::vector<std::string> names();
};

} // namespace saida
