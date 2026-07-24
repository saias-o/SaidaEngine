#pragma once

// game.saida — the boot manifest of a packaged game, written by the editor's
// Build and read by the players (desktop, web). Key=value text:
//
//   project=MyGame.saidaproj
//   main_scene=scenes/main.scene
//
// The web player boots from exactly the same file as the desktop
// executable.

#include <istream>
#include <string>

namespace saida {

struct BootManifest {
    int schema = 1;
    std::string project;    // path to the .saidaproj, relative to the package root
    std::string mainScene;  // path to the starting scene, relative likewise
};

struct BootManifestResult {
    bool ok = false;
    BootManifest manifest;
    std::string error;  // stable message, empty if ok
};

BootManifestResult parseBootManifest(std::istream& in);
BootManifestResult loadBootManifest(const std::string& path);

} // namespace saida
