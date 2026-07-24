#include "core/Paths.hpp"
#include "core/Log.hpp"
#include "scripting/JsRuntime.hpp"
#include "scripting/ScriptBehaviour.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void writeFile(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out.is_open());
    out << contents;
    assert(out.good());
}

bool containsLogLine(const std::vector<std::string>& lines,
                     const std::string& first,
                     const std::string& second) {
    for (const std::string& line : lines) {
        if (line.find(first) != std::string::npos &&
            line.find(second) != std::string::npos)
            return true;
    }
    return false;
}

std::size_t countLogLines(const std::vector<std::string>& lines,
                          const std::string& text) {
    std::size_t count = 0;
    for (const std::string& line : lines) {
        if (line.find(text) != std::string::npos) ++count;
    }
    return count;
}

} // namespace

int main() {
    namespace fs = std::filesystem;

    // A hostile synchronous loop must be interrupted without freezing a frame
    // or the host process.
    {
        saida::JsRuntime runtime;
        auto context = runtime.createContext();
        const auto started = std::chrono::steady_clock::now();
        assert(!context->eval("while (true) {}", "infinite-loop.js"));
        const auto elapsed = std::chrono::steady_clock::now() - started;
        assert(elapsed < std::chrono::seconds(2));
    }

    // Promise jobs are also bounded. A self-replicating microtask chain must
    // fail the evaluation instead of monopolizing every future frame.
    {
        saida::JsRuntime runtime;
        auto context = runtime.createContext();
        assert(!context->eval(R"JS(
            function spin() { Promise.resolve().then(spin); }
            spin();
        )JS", "pending-jobs.js"));
    }

    const fs::path sandbox =
        fs::temp_directory_path() / "saida-js-runtime-safety";
    const fs::path project = sandbox / "project";
    const fs::path outside = sandbox / "outside.mjs";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    writeFile(project / "scripts" / "dep.mjs", "export const value = 42;\n");
    writeFile(project / "scripts" / "no-hooks.mjs", "export const value = 42;\n");
    writeFile(project / "scripts" / "invalid-hook.mjs", "export const onUpdate = 42;\n");
    writeFile(project / "scripts" / "valid-hook.mjs",
              "export function onUpdate() { console.log('valid-hook-ran'); }\n");
    writeFile(outside, "export const secret = 7;\n");
    const fs::path linkedOutside = project / "scripts" / "linked-outside.mjs";
    fs::create_symlink(outside, linkedOutside, ec);
    const bool symlinkCreated = !ec;
    ec.clear();

    const std::string previousProjectRoot = saida::activeProjectRoot();
    saida::setActiveProjectRoot(project.string());
    {
        saida::JsRuntime runtime;

        auto allowed = runtime.createContext();
        assert(allowed->evalModule(
            "import { value } from './dep.mjs';\n"
            "if (value !== 42) throw new Error('bad import');\n",
            (project / "scripts" / "main.mjs").string()));

        auto escaped = runtime.createContext();
        assert(!escaped->evalModule(
            "import { secret } from '../../outside.mjs';\n"
            "globalThis.secret = secret;\n",
            (project / "scripts" / "escape.mjs").string()));

        if (symlinkCreated) {
            auto linked = runtime.createContext();
            assert(!linked->evalModule(
                "import { secret } from './linked-outside.mjs';\n"
                "globalThis.secret = secret;\n",
                (project / "scripts" / "linked.mjs").string()));
        }

        // The primary ScriptBehaviour file follows the same confinement rule,
        // not just its transitive imports.
        saida::ScriptBehaviour script;
        script.setScriptPath(outside.string());
        assert(!script.reload());

        saida::ScriptBehaviour noHooks;
        noHooks.setScriptPath("scripts/no-hooks.mjs");
        assert(noHooks.reload());
        assert(containsLogLine(saida::Log::recent(), "no recognized lifecycle hook",
                               "scripts/no-hooks.mjs"));

        saida::ScriptBehaviour invalidHook;
        invalidHook.setScriptPath("scripts/invalid-hook.mjs");
        assert(invalidHook.reload());
        assert(containsLogLine(saida::Log::recent(), "onUpdate",
                               "scripts/invalid-hook.mjs"));

        saida::ScriptBehaviour validHook;
        validHook.setScriptPath("scripts/valid-hook.mjs");
        validHook.onReady();
        validHook.onUpdate(0.0f);
        assert(!containsLogLine(saida::Log::recent(), "no recognized lifecycle hook",
                                "scripts/valid-hook.mjs"));

        validHook.setScriptPath("scripts/missing-hook.mjs");
        assert(!validHook.reload());
        validHook.onUpdate(0.0f);
        assert(countLogLines(saida::Log::recent(), "valid-hook-ran") == 2);
    }
    saida::setActiveProjectRoot(previousProjectRoot);
    fs::remove_all(sandbox, ec);

    return 0;
}
