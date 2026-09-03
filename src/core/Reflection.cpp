#include "core/Reflection.hpp"

#include <algorithm>

namespace saida::reflect {

TypeRegistry& TypeRegistry::instance() {
    static TypeRegistry registry;
    return registry;
}

TypeDesc& TypeRegistry::add(const std::string& name) {
    auto& slot = types_[name];
    if (!slot) slot = std::make_unique<TypeDesc>();
    return *slot;
}

const TypeDesc* TypeRegistry::find(const std::string& name) const {
    auto it = types_.find(name);
    return it != types_.end() ? it->second.get() : nullptr;
}

bool valueMatchesKind(const PropertyDesc& prop, const json& v, std::string& why) {
    const std::string& kind = prop.kind;
    if (kind == "bool" && !v.is_boolean()) {
        why = "expected bool";
        return false;
    }
    if (kind == "float" && !v.is_number()) {
        why = "expected number";
        return false;
    }
    if (kind == "int" && !v.is_number_integer()) {
        why = "expected integer";
        return false;
    }
    if ((kind == "string" || kind == "asset") && !v.is_string()) {
        why = "expected string";
        return false;
    }
    if (kind == "vec3" && (!v.is_array() || v.size() != 3)) {
        why = "expected vec3 array";
        return false;
    }
    if (kind == "vec4" && (!v.is_array() || v.size() != 4)) {
        why = "expected vec4 array";
        return false;
    }
    if (kind == "quat" && (!v.is_array() || v.size() != 4)) {
        why = "expected quat array";
        return false;
    }
    if (kind == "enum") {
        if (!v.is_number_integer()) {
            why = "expected enum integer";
            return false;
        }
        const int index = v.get<int>();
        if (!prop.enumLabels.empty() &&
            (index < 0 || index >= static_cast<int>(prop.enumLabels.size()))) {
            why = "enum value out of range";
            return false;
        }
    }
    return true;
}

json TypeDesc::manifest() const {
    json j;
    j["name"] = name;
    if (!doc.empty()) j["doc"] = doc;

    if (!properties.empty()) {
        json props = json::array();
        for (const auto& p : properties) {
            json pj;
            pj["name"] = p.name;
            pj["kind"] = p.kind;
            if (!p.tooltip.empty()) pj["doc"] = p.tooltip;
            if (!p.group.empty()) pj["group"] = p.group;
            if (p.hasRange) { pj["min"] = p.min; pj["max"] = p.max; }
            if (!p.enumLabels.empty()) pj["values"] = p.enumLabels;
            props.push_back(std::move(pj));
        }
        j["properties"] = std::move(props);
    }
    if (!signals.empty()) {
        json arr = json::array();
        for (const auto& s : signals) arr.push_back({{"name", s.name}, {"arity", s.arity}});
        j["signals"] = std::move(arr);
    }
    if (!slots.empty()) {
        json arr = json::array();
        for (const auto& s : slots) arr.push_back({{"name", s.name}, {"arity", s.arity}});
        j["slots"] = std::move(arr);
    }
    return j;
}

json TypeRegistry::manifest(const std::string& categoryFilter) const {
    // Stable output makes manifests diff-friendly and cacheable.
    std::vector<const TypeDesc*> behaviours, nodes;
    for (const auto& [name, d] : types_) {
        if (!categoryFilter.empty() && d->category != categoryFilter) continue;
        if (d->category == "node") nodes.push_back(d.get());
        else behaviours.push_back(d.get());
    }
    auto byName = [](const TypeDesc* a, const TypeDesc* b) { return a->name < b->name; };
    std::sort(behaviours.begin(), behaviours.end(), byName);
    std::sort(nodes.begin(), nodes.end(), byName);

    json out;
    if (categoryFilter.empty() || categoryFilter == "behaviour") {
        json arr = json::array();
        for (const auto* d : behaviours) arr.push_back(d->manifest());
        out["behaviours"] = std::move(arr);
    }
    if (categoryFilter.empty() || categoryFilter == "node") {
        json arr = json::array();
        for (const auto* d : nodes) arr.push_back(d->manifest());
        out["nodes"] = std::move(arr);
    }
    return out;
}

json TypeRegistry::manifestFor(const std::string& typeName) const {
    if (const TypeDesc* d = find(typeName)) return d->manifest();
    return json(nullptr);
}

} // namespace saida::reflect
