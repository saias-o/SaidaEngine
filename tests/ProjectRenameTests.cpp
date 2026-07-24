#include "project/ProjectRename.hpp"

#include "core/FormatVersions.hpp"
#include "project/Project.hpp"

#include <nlohmann/json.hpp>

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
        std::cerr << "[project-rename] FAIL: " << what << "\n";
        std::abort();
    }
}

fs::path freshSandbox(const char* label) {
    const fs::path root = fs::temp_directory_path() / "SaidaProjectRenameTests" / label;
    fs::remove_all(root);
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

// Minimal current-schema project on disk, without Project::create's registry
// and audio side effects.
fs::path makeProject(const fs::path& parent, const std::string& name) {
    const fs::path root = parent / name;
    fs::create_directories(root / "scenes");
    json doc;
    format::writeSchema(doc, format::kProjectVersion);
    doc["name"] = name;
    doc["engineVersion"] = kEngineVersion;
    writeFile(root / (name + ".saidaproj"), doc.dump(2) + "\n");
    return root;
}

fs::path makeHubJson(const fs::path& parent, const std::string& name,
                     const fs::path& projectRoot) {
    const fs::path hub = parent / "hub.json";
    json doc;
    doc["projects"] = json::array({
        json{{"name", name}, {"path", projectRoot.string()}},
    });
    writeFile(hub, doc.dump(4));
    return hub;
}

void testNameValidation() {
    std::string error;
    require(isValidProjectName("MyGame", error), "plain name accepted");
    require(isValidProjectName("My Game 2", error), "inner spaces accepted");
    require(!isValidProjectName("", error), "empty name refused");
    require(!isValidProjectName(".", error), "'.' refused");
    require(!isValidProjectName("..", error), "'..' refused");
    require(!isValidProjectName("a/b", error), "slash refused");
    require(!isValidProjectName("a\\b", error), "backslash refused");
    require(!isValidProjectName(" x", error), "leading space refused");
    require(!isValidProjectName("x.", error), "trailing dot refused");
    require(!isValidProjectName("a:b", error), "colon refused");
    require(!isValidProjectName(std::string(129, 'a'), error), "129 chars refused");
    require(isValidProjectName(std::string(128, 'a'), error), "128 chars accepted");
}

void testDirectoryResolution() {
    const fs::path sandbox = freshSandbox("resolve");
    const fs::path root = makeProject(sandbox, "Solo");

    std::string error;
    require(resolveProjectFileInDirectory(root.string(), error) ==
                (root / "Solo.saidaproj").string(),
            "single project file resolved");

    const fs::path empty = sandbox / "Empty";
    fs::create_directories(empty);
    require(resolveProjectFileInDirectory(empty.string(), error).empty(),
            "empty directory refused");

    writeFile(root / "Other.saidaproj", "{}");
    require(resolveProjectFileInDirectory(root.string(), error).empty(),
            "two project files refused as ambiguous");
}

void testSuccessfulRename() {
    const fs::path sandbox = freshSandbox("success");
    const fs::path root = makeProject(sandbox, "Alpha");
    const fs::path hub = makeHubJson(sandbox, "Alpha", root);

    const ProjectRenameResult renamed =
        renameProjectDirectory(root.string(), "Beta", hub.string());
    require(renamed.ok, "rename succeeds");

    const fs::path newRoot = sandbox / "Beta";
    require(renamed.rootPath == newRoot.string(), "result reports new root");
    require(renamed.projectFilePath == (newRoot / "Beta.saidaproj").string(),
            "result reports new project file");
    require(!fs::exists(root), "old directory gone");
    require(fs::exists(newRoot / "Beta.saidaproj"), "project file renamed");
    require(!fs::exists(newRoot / "Alpha.saidaproj"), "old project file gone");

    const json doc = json::parse(readFile(newRoot / "Beta.saidaproj"));
    require(doc["name"] == "Beta", "name field rewritten");
    require(doc["schema"] == format::kProjectVersion, "schema preserved");

    const json hubDoc = json::parse(readFile(hub));
    require(hubDoc["projects"][0]["name"] == "Beta", "hub entry name updated");
    require(hubDoc["projects"][0]["path"] == newRoot.string(),
            "hub entry path updated");

    // The renamed project loads from its directory (the path the Hub stores).
    Project project;
    require(project.load(newRoot.string()), "renamed project loads from directory");
    require(project.name() == "Beta", "loaded name matches");
    require(project.rootPath() == newRoot.string(), "loaded root matches");
}

void testRenameToSameNameNormalizes() {
    const fs::path sandbox = freshSandbox("same-name");
    const fs::path root = makeProject(sandbox, "Alpha");

    // Desynchronize the name field, as the pre-fix editor field could.
    json doc = json::parse(readFile(root / "Alpha.saidaproj"));
    doc["name"] = "Stale";
    writeFile(root / "Alpha.saidaproj", doc.dump(2) + "\n");

    const ProjectRenameResult renamed =
        renameProjectDirectory(root.string(), "Alpha", "");
    require(renamed.ok, "same-name rename succeeds");
    require(json::parse(readFile(root / "Alpha.saidaproj"))["name"] == "Alpha",
            "name field normalized to the directory name");
}

void testRefusalsLeaveDiskUntouched() {
    const fs::path sandbox = freshSandbox("refusals");
    const fs::path root = makeProject(sandbox, "Alpha");
    const fs::path hub = makeHubJson(sandbox, "Alpha", root);
    const std::string projBytes = readFile(root / "Alpha.saidaproj");
    const std::string hubBytes = readFile(hub);

    const auto unchanged = [&](const char* what) {
        require(fs::exists(root / "Alpha.saidaproj"), what);
        require(readFile(root / "Alpha.saidaproj") == projBytes, what);
        require(readFile(hub) == hubBytes, what);
    };

    require(!renameProjectDirectory(root.string(), "a/b", hub.string()).ok,
            "invalid name refused");
    unchanged("invalid name leaves disk untouched");

    fs::create_directories(sandbox / "Taken");
    require(!renameProjectDirectory(root.string(), "Taken", hub.string()).ok,
            "existing target directory refused");
    unchanged("existing target leaves disk untouched");

    require(!renameProjectDirectory((sandbox / "Missing").string(), "Beta",
                                    hub.string())
                 .ok,
            "missing project directory refused");

    // A corrupt hub registry refuses the rename before any mutation.
    const fs::path badHub = sandbox / "bad-hub.json";
    writeFile(badHub, "{ not json");
    require(!renameProjectDirectory(root.string(), "Beta", badHub.string()).ok,
            "corrupt hub registry refused");
    unchanged("corrupt hub registry leaves disk untouched");
}

void testHubWithoutMatchingEntry() {
    const fs::path sandbox = freshSandbox("hub-mismatch");
    const fs::path root = makeProject(sandbox, "Alpha");
    const fs::path hub =
        makeHubJson(sandbox, "Other", sandbox / "SomewhereElse");
    const std::string hubBytes = readFile(hub);

    const ProjectRenameResult renamed =
        renameProjectDirectory(root.string(), "Beta", hub.string());
    require(renamed.ok, "rename succeeds without a matching hub entry");
    require(readFile(hub) == hubBytes, "unrelated hub entries untouched");
}

#ifdef _WIN32
void testDirectoryRenameFailureRollsBack() {
    const fs::path sandbox = freshSandbox("rollback");
    const fs::path root = makeProject(sandbox, "Alpha");
    const fs::path hub = makeHubJson(sandbox, "Alpha", root);
    const std::string projBytes = readFile(root / "Alpha.saidaproj");
    const std::string hubBytes = readFile(hub);

    // An open handle inside the directory makes the directory rename fail on
    // Windows, after the project file was already renamed and rewritten.
    std::ofstream lock(root / "scenes" / "lock.tmp", std::ios::binary);
    lock << "held";
    lock.flush();

    const ProjectRenameResult renamed =
        renameProjectDirectory(root.string(), "Beta", hub.string());
    require(!renamed.ok, "locked directory rename fails");

    require(fs::exists(root / "Alpha.saidaproj"), "project file name rolled back");
    require(!fs::exists(root / "Beta.saidaproj"), "renamed project file gone");
    require(readFile(root / "Alpha.saidaproj") == projBytes,
            "project file bytes rolled back");
    require(readFile(hub) == hubBytes, "hub registry untouched after rollback");
    require(fs::exists(root) && !fs::exists(sandbox / "Beta"),
            "directory kept its old name");
}
#endif

} // namespace

int main() {
    testNameValidation();
    testDirectoryResolution();
    testSuccessfulRename();
    testRenameToSameNameNormalizes();
    testRefusalsLeaveDiskUntouched();
    testHubWithoutMatchingEntry();
#ifdef _WIN32
    testDirectoryRenameFailureRollsBack();
#endif

    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / "SaidaProjectRenameTests", ec);
    std::cout << "[project-rename] PASS (" << gChecks << " checks)\n";
    return 0;
}
