// Durable content the engine cannot read must be REFUSED, never quietly
// replaced. Both cases below used to be — or could still become — a silent
// rewrite of a document the user cannot get back.
//
// The registry case was live: Project::load discarded AssetRegistry::load's
// verdict and ran sync() + save() regardless, so a registry the schema guard
// refused was overwritten by a fresh scan with brand new ids, leaving every
// AssetID a scene had stored pointing at nothing. The decisive assertion here is
// not that the load fails — it is that the bytes on disk are still there
// afterwards, because a refusal that still destroys the file fixes nothing.
//
// The scene case is a guard rather than a repair: every public entry point
// validates the type contract over the whole tree before deserializing, so a
// nested unknown type is already refused. The test pins that, so the belt in
// deserializeNode and this validator cannot both be removed by someone who
// checked only the other one.

#include "core/FormatVersions.hpp"
#include "project/Project.hpp"
#include "scene/Node.hpp"
#include "scene/NodeRegistry.hpp"
#include "scene/ReflectedTypes.hpp"
#include "scene/SceneSerializer.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace saida;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

int gChecks = 0;

void require(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        std::cerr << "[durable-load] FAIL: " << what << "\n";
        std::abort();
    }
}

fs::path freshSandbox(const char* label) {
    const fs::path root = fs::temp_directory_path() / "SaidaDurableLoadTests" / label;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    return root;
}

std::string readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

void writeFile(const fs::path& path, const std::string& contents) {
    std::ofstream file(path, std::ios::binary);
    file << contents;
}

// A current-schema project on disk with one asset file to be scanned.
fs::path makeProject(const fs::path& parent, const std::string& name) {
    const fs::path root = parent / name;
    fs::create_directories(root / "assets");
    writeFile(root / "assets" / "block.png", "not really a png, only a file to find");

    json doc;
    format::writeSchema(doc, format::kProjectVersion);
    doc["name"] = name;
    doc["engineVersion"] = kEngineVersion;
    writeFile(root / (name + ".saidaproj"), doc.dump(2) + "\n");
    return root;
}

std::string projectFile(const fs::path& root, const std::string& name) {
    return (root / (name + ".saidaproj")).string();
}

// A registry whose envelope the schema guard refuses: well-formed JSON, wrong
// schema version. This is the shape the guard exists to catch, as opposed to
// unparseable bytes.
std::string refusedRegistry() {
    json doc;
    format::writeSchema(doc, format::kAssetRegistryVersion + 7);
    doc["assets"] = json::object({
        {"424242", json{{"path", "assets/block.png"}, {"hash", 99ULL}, {"type", "Texture"}}},
    });
    return doc.dump(4);
}

std::string validRegistry() {
    json doc;
    format::writeSchema(doc, format::kAssetRegistryVersion);
    doc["assets"] = json::object({
        {"424242", json{{"path", "assets/block.png"}, {"hash", 99ULL}, {"type", "Texture"}}},
    });
    return doc.dump(4);
}

// The whole point: a registry the engine refuses is still on disk afterwards.
void testRefusedRegistryIsNotOverwritten() {
    const fs::path sandbox = freshSandbox("refused-registry");
    const fs::path root = makeProject(sandbox, "Refused");
    const fs::path registry = root / "asset_registry.json";

    writeFile(registry, refusedRegistry());
    const std::string before = readFile(registry);

    Project project;
    require(!project.load(projectFile(root, "Refused")),
            "a project whose registry cannot be read is refused");
    require(readFile(registry) == before,
            "the refused registry is byte-identical on disk after the refusal");
    require(!project.isLoaded(), "the refused project is not left half-loaded");
}

// A refusal must not be reached by making every unreadable registry unreadable:
// the ordinary project still has to load, and keep the ids it was given.
void testValidRegistryStillLoads() {
    const fs::path sandbox = freshSandbox("valid-registry");
    const fs::path root = makeProject(sandbox, "Valid");

    writeFile(root / "asset_registry.json", validRegistry());

    Project project;
    require(project.load(projectFile(root, "Valid")), "a valid project loads");
    require(project.isLoaded(), "the valid project reports itself loaded");
    require(project.assetRegistry().getID("assets/block.png") == 424242,
            "the id recorded in the registry survives the load");
}

// Absence is not corruption: a project without a registry is how a new one
// starts, and it must still be scanned and written.
void testMissingRegistryIsCreated() {
    const fs::path sandbox = freshSandbox("missing-registry");
    const fs::path root = makeProject(sandbox, "Fresh");
    const fs::path registry = root / "asset_registry.json";
    require(!fs::exists(registry), "the fixture starts without a registry");

    Project project;
    require(project.load(projectFile(root, "Fresh")),
            "a project without a registry still loads");
    require(fs::exists(registry), "the missing registry is created by the load");
    require(project.assetRegistry().getID("assets/block.png") != kAssetInvalid,
            "the scan registered the asset that was on disk");
}

// A scene document whose unknown type sits two levels down, which is the case a
// depth-1 check would miss.
std::string sceneWithNestedUnknownType() {
    json leaf;
    leaf["type"] = "Enemyy";  // the typo this whole guard exists for
    leaf["name"] = "Boss";

    json middle;
    middle["type"] = "Node";
    middle["name"] = "Wave2";
    middle["children"] = json::array({leaf});

    json top;
    top["type"] = "Node";
    top["name"] = "Enemies";
    top["children"] = json::array({middle});

    json scene;
    scene["type"] = "Node";
    scene["name"] = "Level";
    scene["children"] = json::array({top});
    return scene.dump(2);
}

void testNestedUnknownTypeIsRefused() {
    // The validator answers from the node registry, and a headless test has to
    // populate it: "Node" and "Scene" are registered by Engine::init, and there
    // is no Engine here. "Node" is all this fixture needs.
    registerReflectedTypes();
    auto& nodes = NodeRegistry::instance();
    if (nodes.factories().find("Node") == nodes.factories().end())
        nodes.registerType<Node>("Node");

    std::string error;
    require(!SceneSerializer::validateTypeContractJson(sceneWithNestedUnknownType(), &error),
            "an unknown node type two levels down is refused");
    require(error.find("Enemyy") != std::string::npos,
            "the refusal names the offending type");
    require(error.find("children[0].children[0]") != std::string::npos,
            "the refusal locates the offending node in the tree");

    json ok = json::parse(sceneWithNestedUnknownType());
    ok["children"][0]["children"][0]["children"][0]["type"] = "Node";
    std::string unused;
    require(SceneSerializer::validateTypeContractJson(ok.dump(2), &unused),
            "the same tree with a known type is accepted");
}

} // namespace

int main() {
    testRefusedRegistryIsNotOverwritten();
    testValidRegistryStillLoads();
    testMissingRegistryIsCreated();
    testNestedUnknownTypeIsRefused();

    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / "SaidaDurableLoadTests", ec);
    std::cout << "[durable-load] PASS (" << gChecks << " checks)\n";
    return 0;
}
