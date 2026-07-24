#include "project/Project.hpp"
#include "runtime/TestAutoload.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>

int main() {
    namespace fs = std::filesystem;
    const fs::path parent = fs::temp_directory_path() / "SaidaTestAutoloadTests";
    std::error_code ec;
    fs::remove_all(parent, ec);
    fs::create_directories(parent, ec);
    assert(!ec);

    saida::Project project;
    assert(project.create(parent.string(), "Game"));
    const fs::path script = fs::path(project.scriptsDir()) / "driver.js";
    std::ofstream(script) << "function onReady() {}\n";

    std::string error;
    assert(saida::runtime::applyTestAutoload(
        project, "Driver=scripts/driver.js", error));
    assert(project.autoloads().at("Driver") == "scripts/driver.js");

    error.clear();
    assert(!saida::runtime::applyTestAutoload(
        project, "bad name=scripts/driver.js", error));
    assert(!error.empty());

    error.clear();
    assert(!saida::runtime::applyTestAutoload(
        project, "Driver=../driver.js", error));
    assert(!error.empty());

    error.clear();
    assert(!saida::runtime::applyTestAutoload(
        project, "Driver=scripts/missing.js", error));
    assert(!error.empty());

    error.clear();
    assert(!saida::runtime::applyTestAutoload(
        project, "Driver=Game.saidaproj", error));
    assert(!error.empty());

    fs::remove_all(parent, ec);
    return 0;
}
