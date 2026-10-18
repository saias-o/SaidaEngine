#include "core/PlatformCaps.hpp"
#include "runtime/BootManifest.hpp"
#include "runtime/ProjectArgs.hpp"

#include <cassert>
#include <sstream>
#include <string>
#include <vector>

namespace {

void testBootManifestParse() {
    std::istringstream in(
        "# packaged by the editor\n"
        "schema = 1\n"
        "project = MyGame.saidaproj\n"
        "\n"
        "main_scene=scenes/main.scene\n"
        "unknown_key=ignored\n");
    const auto result = saida::parseBootManifest(in);
    assert(result.ok);
    assert(result.manifest.schema == 1);
    assert(result.manifest.project == "MyGame.saidaproj");
    assert(result.manifest.mainScene == "scenes/main.scene");
}

void testBootManifestErrors() {
    std::istringstream noProject("schema=1\nmain_scene=scenes/main.scene\n");
    auto r1 = saida::parseBootManifest(noProject);
    assert(!r1.ok);
    assert(r1.error.find("project") != std::string::npos);

    std::istringstream noScene("schema=1\nproject=MyGame.saidaproj\n");
    auto r2 = saida::parseBootManifest(noScene);
    assert(!r2.ok);
    assert(r2.error.find("main_scene") != std::string::npos);

    auto r3 = saida::loadBootManifest("does/not/exist/game.saida");
    assert(!r3.ok);

    std::istringstream future(
        "schema=99\nproject=MyGame.saidaproj\nmain_scene=scenes/main.scene\n");
    auto r4 = saida::parseBootManifest(future);
    assert(!r4.ok);
}

void testPlatformCaps() {
    using namespace saida::platform;

    // Default: everything available (existing executables are unaffected).
    setCapabilities(kAllCapabilities);
    assert(has(Capability::Physics));
    assert(require(Capability::Physics, "test"));

    // A restricted player: missing capability -> require() false, reported once.
    setCapabilities(kAllCapabilities & ~uint32_t(Capability::Physics) &
                    ~uint32_t(Capability::Audio));
    assert(!has(Capability::Physics));
    assert(has(Capability::Rendering));
    assert(!require(Capability::Physics, "RigidBody"));
    assert(!require(Capability::Physics, "RigidBody"));  // deduplicated, no spam

    const std::string report = saida::platform::report();
    assert(report.find("physics=NO") != std::string::npos);
    assert(report.find("audio=NO") != std::string::npos);
    assert(report.find("rendering=yes") != std::string::npos);

    setCapabilities(kAllCapabilities);  // avoid polluting other tests
}

} // namespace

int main() {
    auto parse = [](std::vector<std::string> args, bool expected, std::string value = "") {
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        std::string project, error;
        assert(saida::runtime::parseProjectArgs(int(argv.size()), argv.data(), project, error) == expected);
        if (expected) assert(project == value);
        else assert(!error.empty());
    };
    parse({"game"}, true);
    parse({"game", "--project", "A game & tools/game.saidaproj"}, true, "A game & tools/game.saidaproj");
    parse({"game", "--project"}, false);
    parse({"game", "--project", "--profile"}, false);
    parse({"game", "--project", "a", "--project", "b"}, false);
    testBootManifestParse();
    testBootManifestErrors();
    testPlatformCaps();
    return 0;
}
