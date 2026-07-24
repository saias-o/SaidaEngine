#include "core/FormatVersions.hpp"
#include "project/AssetRegistry.hpp"
#include "project/Project.hpp"
#include "scenario/ScenarioAsset.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using json = nlohmann::json;

namespace {

void require(bool condition) {
    if (!condition) std::abort();
}

std::filesystem::path testRoot() {
    auto root = std::filesystem::temp_directory_path() / "SaidaEngineFormatVersionTests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path);
    file << text;
}

json readJson(const std::filesystem::path& path) {
    std::ifstream file(path);
    return json::parse(file);
}

json baseScenario() {
    return {
        {"schema", saida::format::kScenarioVersion},
        {"version", saida::format::kScenarioVersion},
        {"id", "format.test"},
        {"roles", json::object()},
        {"blackboard", json::object()},
        {"steps", json::array({{{"id", "start"}, {"end", "success"}}})}
    };
}

void testNewProjectUsesCurrentEngineVersion() {
    const auto root = testRoot();
    saida::Project project;
    require(project.create(root.string(), "CurrentProject"));
    require(project.engineVersion() == saida::kEngineVersion);
    require(readJson(root / "CurrentProject" / "CurrentProject.saidaproj")["engineVersion"] ==
            saida::kEngineVersion);
}

void testNonJsonProjectIsRejected() {
    const auto root = testRoot() / "InvalidProject";
    std::filesystem::create_directories(root);
    const auto path = root / "InvalidProject.saidaproj";
    writeText(path, "name=InvalidProject\n");
    saida::Project project;
    require(!project.load(path.string()));
}

void testWrongProjectSchemaIsRejected() {
    const auto root = testRoot() / "WrongProject";
    std::filesystem::create_directories(root);
    const auto path = root / "WrongProject.saidaproj";
    writeText(path, json{
        {"schema", 99}, {"version", 99}, {"name", "WrongProject"},
        {"engineVersion", saida::kEngineVersion}
    }.dump(2));
    saida::Project project;
    require(!project.load(path.string()));
}

void testCurrentAssetRegistryLoads() {
    const auto root = testRoot() / "RegistryProject";
    std::filesystem::create_directories(root);
    writeText(root / "asset_registry.json", json{
        {"schema", saida::format::kAssetRegistryVersion},
        {"version", saida::format::kAssetRegistryVersion},
        {"assets", {{"123", {{"path", "scenes/main.scene"},
                                {"hash", 42}, {"type", "Scene"}}}}}
    }.dump(2));
    saida::AssetRegistry registry;
    require(registry.load(root.string()));
    require(registry.getID("scenes/main.scene") == 123);
}

void testWrongAssetRegistrySchemaIsRejected() {
    const auto root = testRoot() / "WrongRegistry";
    std::filesystem::create_directories(root);
    writeText(root / "asset_registry.json", json{
        {"schema", 99}, {"version", 99}, {"assets", json::object()}
    }.dump(2));
    saida::AssetRegistry registry;
    require(!registry.load(root.string()));
}

void testScenarioRequiresCurrentEnvelope() {
    saida::ScenarioAsset asset;
    std::vector<saida::ScenarioIssue> issues;
    require(saida::ScenarioAsset::parse(baseScenario(), asset, &issues));

    json missing = baseScenario();
    missing.erase("schema");
    issues.clear();
    require(!saida::ScenarioAsset::parse(missing, asset, &issues));

    json wrong = baseScenario();
    wrong["schema"] = 99;
    wrong["version"] = 99;
    issues.clear();
    require(!saida::ScenarioAsset::parse(wrong, asset, &issues));
}

void testSchemaEnvelopeHelperIsStrict() {
    using saida::format::schemaEnvelopeError;
    const int current = 2;
    require(schemaEnvelopeError(json{{"schema", 2}, {"version", 2}}, current, "scene").empty());
    require(schemaEnvelopeError(json{{"version", 2}}, current, "scene").find("schema is required") !=
            std::string::npos);
    require(schemaEnvelopeError(json{{"schema", 2}}, current, "scene").find("version is required") !=
            std::string::npos);
    require(schemaEnvelopeError(json{{"schema", 1}, {"version", 1}}, current, "scene")
                .find("unsupported scene schema v1") != std::string::npos);
    require(schemaEnvelopeError(json{{"schema", 2}, {"version", 1}}, current, "scene")
                .find("schema/version mismatch") != std::string::npos);
}

} // namespace

int main() {
    testNewProjectUsesCurrentEngineVersion();
    testNonJsonProjectIsRejected();
    testWrongProjectSchemaIsRejected();
    testCurrentAssetRegistryLoads();
    testWrongAssetRegistrySchemaIsRejected();
    testScenarioRequiresCurrentEnvelope();
    testSchemaEnvelopeHelperIsStrict();
    return 0;
}
