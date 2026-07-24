#include "core/Paths.hpp"
#include "scripting/ScriptBehaviour.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>

int main() {
    namespace fs = std::filesystem;
    using saida::ScriptCallStatus;

    const std::string previousRoot = saida::activeProjectRoot();
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("saida_js_cross_context_" + std::to_string(unique));
    fs::create_directories(root);
    {
        std::ofstream script(root / "bridge.mjs", std::ios::binary);
        script << R"JS(
            export function sum(a, b) { return a + b; }
            export function echo(value) { return value; }
            export function explode() { throw new Error("expected bridge failure"); }
        )JS";
    }

    saida::setActiveProjectRoot(root.string());
    saida::ScriptBehaviour behaviour;
    behaviour.setScriptPath("bridge.mjs");

    nlohmann::json result;
    assert(behaviour.callExport("sum", nlohmann::json::array({2, 3}), result) ==
           ScriptCallStatus::Succeeded);
    assert(result == 5);

    const nlohmann::json payload = {
        {"name", "Witness"},
        {"values", nlohmann::json::array({1, true, "three"})},
        {"nested", {{"ready", true}}},
    };
    assert(behaviour.callExport("echo", nlohmann::json::array({payload}), result) ==
           ScriptCallStatus::Succeeded);
    assert(result == payload);

    assert(behaviour.callExport("missing", nlohmann::json::array(), result) ==
           ScriptCallStatus::Missing);
    assert(behaviour.callExport("explode", nlohmann::json::array(), result) ==
           ScriptCallStatus::Failed);

    saida::setActiveProjectRoot(previousRoot);
    std::error_code ec;
    fs::remove_all(root, ec);
    return 0;
}
