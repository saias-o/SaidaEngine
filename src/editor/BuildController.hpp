#pragma once

#include "editor/EditorEnums.hpp"

#include <string>
#include <vector>

namespace saida {

class Project;

// Owns the Build Settings modal and the export workflow shared by the modal
// buttons and the --build automation entry point.
//
// Invariant: both interactive and automated exports end in executeBuild(), so
// scene selection, exporter options, and result reporting cannot drift apart.
class BuildController {
public:
    void requestOpen() { openRequested_ = true; }
    void draw(Project* project);

    bool runAutomatedBuild(Project* project, bool web,
                           const std::string& outputDir,
                           std::string* message);

private:
    void refreshScenes(Project* project);
    void executeBuild(Project* project, bool web, bool launchAfter);
    void drawPlatformList(float bottomReserve);
    void drawPlatformSettings(float bottomReserve);
    void drawStartupScene();
    void drawWindowsSettings();
    void drawQuestSettings();
    void drawLinuxSettings();
    void drawWebSettings();
    void drawResult();
    void drawFooter(Project* project);

    bool openRequested_ = false;
    BuildPlatform selectedPlatform_ = BuildPlatform::Windows;
    BuildConfig configuration_ = BuildConfig::Release;
    char outputPath_[512] = "build/export";
    char version_[32] = "1.0.0";
    char company_[256] = "";
    char iconPath_[512] = "";
    bool copyAssets_ = true;
    bool enableLto_ = false;

    std::vector<std::string> scenes_;
    int mainSceneIndex_ = 0;
    bool hasResult_ = false;
    bool lastSuccess_ = false;
    std::string lastError_;
    std::string lastLog_;
    std::string lastOutputDir_;
    std::string lastExe_;
};

} // namespace saida
