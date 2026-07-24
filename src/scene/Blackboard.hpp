#pragma once

#include "core/ReflectionFwd.hpp"
#include "core/Signal.hpp"
#include "scene/Behaviour.hpp"

#include "nlohmann/json_fwd.hpp"

#include <string>
#include <unordered_map>
#include <variant>

namespace saida {

// Shared typed key/value store for gameplay state (cf. behaviour-tree blackboard).
// Used as an autoload (persistent, found via tree()->autoload<Blackboard>()) or
// placed on a node in the "blackboard" group. StateMachine reads it; a `changed`
// signal lets reactive logic listen. Values are number/bool/string.
class Blackboard : public Behaviour {
public:
    using Value = std::variant<double, bool, std::string>;

    void setNumber(std::string key, double value);   // slot
    void setBool(std::string key, bool value);        // slot
    void setString(std::string key, std::string value); // slot

    bool has(const std::string& key) const;
    double number(const std::string& key, double fallback = 0.0) const;
    bool boolean(const std::string& key, bool fallback = false) const;
    std::string string(const std::string& key, const std::string& fallback = {}) const;
    // Typed access without fallback games (JS bindings). Null when absent.
    const Value* find(const std::string& key) const;

    Signal<std::string> changed;  // emits the key whenever a value is set

    static constexpr const char* reflectName() { return "Blackboard"; }
    const char* typeName() const override { return "Blackboard"; }
    static void describe(reflect::TypeBuilder<Blackboard>& t);
    void save(nlohmann::json& j) const override;
    void load(const nlohmann::json& j) override;

private:
    std::unordered_map<std::string, Value> values_;
};

} // namespace saida
