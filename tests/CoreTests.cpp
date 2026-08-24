#include "core/Easing.hpp"
#include "core/Paths.hpp"
#include "core/Signal.hpp"

#include <cassert>
#include <cmath>
#include <filesystem>

namespace {

void testSandboxedProjectPaths() {
    const std::string root =
        (std::filesystem::temp_directory_path() / "SaidaSandboxRoot").generic_string();

    auto simple = saida::resolveSandboxedProjectPath(root, "npc.lua", "scripts");
    assert(simple.ok);
    assert(simple.relative == "scripts/npc.lua");
    assert(simple.absolute.size() >= simple.relative.size());

    auto nested = saida::resolveSandboxedProjectPath(root, "scripts/./ai/../npc.lua");
    assert(nested.ok);
    assert(nested.relative == "scripts/npc.lua");

    auto parent = saida::resolveSandboxedProjectPath(root, "../secret.lua");
    assert(!parent.ok);

    auto escaped = saida::resolveSandboxedProjectPath(root, "scripts/../../secret.lua");
    assert(!escaped.ok);

    auto absolute = saida::resolveSandboxedProjectPath(root, "/tmp/secret.lua");
    assert(!absolute.ok);

    auto drive = saida::resolveSandboxedProjectPath(root, "C:/secret.lua");
    assert(!drive.ok);

    auto url = saida::resolveSandboxedProjectPath(root, "file:///C:/secret.lua");
    assert(!url.ok);
}

void testInstalledRootResolution() {
    saida::setRuntimeRoot("C:/Portable/SaidaEngine");
    assert(saida::installedLayout());
    assert(saida::engineRoot() == "C:/Portable/SaidaEngine");
    assert(saida::runtimeBinaryRoot() == "C:/Portable/SaidaEngine");
    assert(saida::shaderRoot() == "C:/Portable/SaidaEngine/shaders");
    assert(saida::assetPath("assets/editor/logo.png") ==
           "C:/Portable/SaidaEngine/assets/editor/logo.png");
    saida::setRuntimeRoot("");
    assert(!saida::installedLayout());
}

} // namespace

int main() {
    assert(std::abs(saida::applyEasing(saida::Easing::Linear, 0.25f) - 0.25f) < 1e-6f);

    saida::Signal<int> signal;
    int observed = 0;
    {
        auto connection = signal.connect([&](int value) { observed = value; });
        signal.emit(7);
        assert(observed == 7);
    }
    signal.emit(9);
    assert(observed == 7);

    testSandboxedProjectPaths();
    testInstalledRootResolution();
    return 0;
}
