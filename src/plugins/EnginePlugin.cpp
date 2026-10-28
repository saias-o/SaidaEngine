#include "plugins/EnginePlugin.hpp"

#include "core/Log.hpp"

#include <algorithm>

namespace saida::plugins {

namespace {

struct Entry {
    EnginePlugin* plugin = nullptr;
    bool active = false;
};

// Function-local: plugins register during static initialization, in any order
// relative to this translation unit.
std::vector<Entry>& registry() {
    static std::vector<Entry> entries;
    return entries;
}

} // namespace

void registerPlugin(EnginePlugin& plugin) {
    auto& entries = registry();
    const std::string_view name = plugin.name();
    const bool duplicate = std::any_of(entries.begin(), entries.end(),
        [&](const Entry& e) { return name == e.plugin->name(); });
    if (!duplicate) entries.push_back({&plugin, false});
}

std::vector<EnginePlugin*> compiled() {
    std::vector<EnginePlugin*> out;
    for (const Entry& e : registry()) out.push_back(e.plugin);
    return out;
}

void activate(const std::vector<std::string>& names) {
    auto& entries = registry();
    for (const std::string& name : names) {
        const bool known = std::any_of(entries.begin(), entries.end(),
            [&](const Entry& e) { return name == e.plugin->name(); });
        if (!known)
            Log::warn("plugins: '", name, "' is enabled by the project but not compiled into this "
                      "executable (add it to SAIDA_PLUGINS)");
    }
    for (Entry& e : entries) {
        const bool wanted = std::find(names.begin(), names.end(), e.plugin->name()) != names.end();
        if (wanted == e.active) continue;
        e.active = wanted;
        if (wanted) {
            Log::info("plugins: activated '", e.plugin->name(), "'");
            e.plugin->onActivate();
        } else {
            e.plugin->onDeactivate();
        }
    }
}

bool isActive(std::string_view name) {
    for (const Entry& e : registry())
        if (e.active && name == e.plugin->name()) return true;
    return false;
}

void installScriptApi(JSContext* ctx, JSValueConst global) {
    for (const Entry& e : registry())
        if (e.active) e.plugin->installScriptApi(ctx, global);
}

} // namespace saida::plugins
