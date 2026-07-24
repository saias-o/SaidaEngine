#include "scene/Blackboard.hpp"

#include "core/Reflection.hpp"

namespace saida {

void Blackboard::setNumber(std::string key, double value) {
    values_[key] = value;
    changed.emit(key);
}
void Blackboard::setBool(std::string key, bool value) {
    values_[key] = value;
    changed.emit(key);
}
void Blackboard::setString(std::string key, std::string value) {
    values_[key] = std::move(value);
    changed.emit(key);
}

bool Blackboard::has(const std::string& key) const { return values_.count(key) != 0; }

const Blackboard::Value* Blackboard::find(const std::string& key) const {
    auto it = values_.find(key);
    return it != values_.end() ? &it->second : nullptr;
}

double Blackboard::number(const std::string& key, double fallback) const {
    auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    if (auto* d = std::get_if<double>(&it->second)) return *d;
    if (auto* b = std::get_if<bool>(&it->second)) return *b ? 1.0 : 0.0;
    return fallback;
}
bool Blackboard::boolean(const std::string& key, bool fallback) const {
    auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    if (auto* b = std::get_if<bool>(&it->second)) return *b;
    if (auto* d = std::get_if<double>(&it->second)) return *d != 0.0;
    return fallback;
}
std::string Blackboard::string(const std::string& key, const std::string& fallback) const {
    auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    if (auto* s = std::get_if<std::string>(&it->second)) return *s;
    return fallback;
}

void Blackboard::describe(reflect::TypeBuilder<Blackboard>& t) {
    t.doc("Shared typed key/value store (number/bool/string). Used as an autoload "
          "or in the 'blackboard' group; emits 'changed' on write.");
    t.signal("changed", &Blackboard::changed);
    t.slot("setNumber", &Blackboard::setNumber);
    t.slot("setBool", &Blackboard::setBool);
    t.slot("setString", &Blackboard::setString);
}

void Blackboard::save(nlohmann::json& j) const {
    // Persist initial values as a typed map: { key: {n|b|s: value} }.
    nlohmann::json values = nlohmann::json::object();
    for (const auto& [key, v] : values_) {
        if (auto* d = std::get_if<double>(&v)) values[key] = {{"n", *d}};
        else if (auto* b = std::get_if<bool>(&v)) values[key] = {{"b", *b}};
        else if (auto* s = std::get_if<std::string>(&v)) values[key] = {{"s", *s}};
    }
    j["values"] = std::move(values);
}

void Blackboard::load(const nlohmann::json& j) {
    values_.clear();
    auto it = j.find("values");
    if (it == j.end() || !it->is_object()) return;
    for (auto& [key, entry] : it->items()) {
        if (entry.contains("n")) values_[key] = entry["n"].get<double>();
        else if (entry.contains("b")) values_[key] = entry["b"].get<bool>();
        else if (entry.contains("s")) values_[key] = entry["s"].get<std::string>();
    }
}

} // namespace saida
