#pragma once

#include <quickjs.h>

#include <string>
#include <string_view>
#include <vector>

namespace saida {

// A runtime plugin: optional engine functionality that lives outside the
// engine sources (plugins/<name>/), is compiled into the executables that list
// it in the SAIDA_PLUGINS CMake option, and only runs for projects that enable
// it in their .saidaproj ("plugins": ["<name>"]). A plugin that is compiled in
// but not enabled costs nothing and exposes nothing.
//
// A plugin registers itself from its own translation unit with
// SAIDA_REGISTER_PLUGIN(Type); the engine never names a plugin.
class EnginePlugin {
public:
    virtual ~EnginePlugin() = default;

    // Identifier used by .saidaproj "plugins" and SAIDA_PLUGINS.
    virtual const char* name() const = 0;

    // The loaded project enables / no longer enables this plugin.
    virtual void onActivate() {}
    virtual void onDeactivate() {}

    // Adds the plugin's script API to a fresh script context: every behaviour
    // script and every UI document of a project that enables the plugin.
    // `global` is that context's global object; only add properties to it.
    virtual void installScriptApi(JSContext* ctx, JSValueConst global) { (void)ctx; (void)global; }
};

namespace plugins {

// Called once per plugin at static initialization (SAIDA_REGISTER_PLUGIN).
void registerPlugin(EnginePlugin& plugin);

// Every plugin compiled into this executable, active or not.
std::vector<EnginePlugin*> compiled();

// Enables exactly the named plugins (Project::load passes .saidaproj
// "plugins"): newly named ones are activated, the others deactivated. Names
// not compiled into this executable are reported and ignored.
void activate(const std::vector<std::string>& names);

bool isActive(std::string_view name);

// Installs the script API of every active plugin into one script context.
void installScriptApi(JSContext* ctx, JSValueConst global);

} // namespace plugins
} // namespace saida

#define SAIDA_PLUGIN_CONCAT_(a, b) a##b
#define SAIDA_PLUGIN_CONCAT(a, b) SAIDA_PLUGIN_CONCAT_(a, b)
#define SAIDA_REGISTER_PLUGIN(Type)                                                    \
    namespace {                                                                        \
    Type SAIDA_PLUGIN_CONCAT(gSaidaPlugin_, __LINE__);                                 \
    [[maybe_unused]] const bool SAIDA_PLUGIN_CONCAT(gSaidaPluginRegistered_, __LINE__) = \
        (::saida::plugins::registerPlugin(SAIDA_PLUGIN_CONCAT(gSaidaPlugin_, __LINE__)), true); \
    }
