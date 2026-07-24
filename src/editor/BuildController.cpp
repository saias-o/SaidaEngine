#include "editor/BuildController.hpp"

#include "editor/BuildExporter.hpp"
#include "editor/ExeMetadata.hpp"
#include "project/Project.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iterator>

namespace saida {

void BuildController::refreshScenes(Project* project) {
    scenes_.clear();
    if (!project || !project->isLoaded()) return;

    namespace fs = std::filesystem;
    std::error_code error;
    const fs::path scenesDir = project->scenesDir();
    if (fs::exists(scenesDir, error)) {
        for (const auto& entry : fs::directory_iterator(scenesDir, error)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() == ".scene")
                scenes_.push_back("scenes/" + entry.path().filename().string());
        }
        std::sort(scenes_.begin(), scenes_.end());
    }

    // Prefer the configured main scene, then the conventional main.scene,
    // then the first scene in lexical order.
    mainSceneIndex_ = 0;
    const std::string preferred[] = {
        project->mainScene(),
        "scenes/main.scene",
    };
    for (const std::string& wanted : preferred) {
        if (wanted.empty()) continue;
        const auto found = std::find(scenes_.begin(), scenes_.end(), wanted);
        if (found == scenes_.end()) continue;
        mainSceneIndex_ =
            static_cast<int>(std::distance(scenes_.begin(), found));
        break;
    }
}

void BuildController::executeBuild(Project* project, bool web,
                                   bool launchAfter) {
    BuildExporter::Options options;
    options.outputDir = outputPath_;
    options.mainScene = scenes_.empty()
        ? std::string("scenes/main.scene")
        : scenes_[mainSceneIndex_];
    options.launchAfterBuild = launchAfter && !web;
    options.productVersion = version_;
    options.companyName = company_;
    options.iconPath = iconPath_;

    BuildExporter::Result result = web
        ? BuildExporter::exportWebBuild(*project, options)
        : BuildExporter::exportWindowsBuild(*project, options);
    hasResult_ = true;
    lastSuccess_ = result.success;
    lastError_ = result.error;
    lastLog_ = result.log;
    lastOutputDir_ = result.outputDir;
    lastExe_ = result.gameExe;
}

bool BuildController::runAutomatedBuild(Project* project, bool web,
                                        const std::string& outputDir,
                                        std::string* message) {
    if (!project || !project->isLoaded()) {
        if (message) *message = "no loaded project";
        return false;
    }

    hasResult_ = false;
    refreshScenes(project);
    if (scenes_.empty()) {
        if (message) *message = "no .scene files under scenes/";
        return false;
    }
    if (!outputDir.empty())
        std::snprintf(outputPath_, sizeof(outputPath_), "%s",
                      outputDir.c_str());

    selectedPlatform_ =
        web ? BuildPlatform::WebGL : BuildPlatform::Windows;
    executeBuild(project, web, /*launchAfter=*/false);
    if (message)
        *message = lastSuccess_ ? lastOutputDir_ : lastError_;
    return lastSuccess_;
}

void BuildController::draw(Project* project) {
    if (openRequested_) {
        ImGui::OpenPopup("Build Settings");
        openRequested_ = false;
        hasResult_ = false;
        refreshScenes(project);
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(680, 560), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Build Settings", nullptr,
                                ImGuiWindowFlags_None))
        return;

    if (project && project->isLoaded()) {
        ImGui::Text("Configure build settings for project: %s",
                    project->name().c_str());
    } else {
        ImGui::Text(
            "Configure build targets, platforms, and optimization parameters.");
    }
    ImGui::Separator();
    ImGui::Spacing();

    float bottomReserve = ImGui::GetFrameHeightWithSpacing() + 10.0f;
    if (hasResult_)
        bottomReserve +=
            90.0f + ImGui::GetFrameHeightWithSpacing() * 2.0f + 16.0f;

    drawPlatformList(bottomReserve);
    ImGui::SameLine();
    drawPlatformSettings(bottomReserve);
    ImGui::Separator();
    ImGui::Spacing();
    drawResult();
    drawFooter(project);
    ImGui::EndPopup();
}

void BuildController::drawPlatformList(float bottomReserve) {
    ImGui::BeginChild("##PlatformList", ImVec2(200, -bottomReserve),
                      ImGuiChildFlags_Borders);
    const char* platforms[] = {
        "Windows (Direct3D/Vulkan)",
        "Meta Quest (XR SDK)",
        "Linux (Vulkan)",
        "WebGL (WebAssembly)",
    };
    for (int i = 0; i < 4; ++i) {
        ImGui::PushID(i);
        const bool selected =
            static_cast<int>(selectedPlatform_) == i;

        char label[64];
        if (i == 0)
            std::snprintf(label, sizeof(label), " [Win] %s", platforms[i]);
        else if (i == 1)
            std::snprintf(label, sizeof(label), " [XR]  %s", platforms[i]);
        else if (i == 2)
            std::snprintf(label, sizeof(label), " [Tux] %s", platforms[i]);
        else
            std::snprintf(label, sizeof(label), " [Web] %s", platforms[i]);

        if (ImGui::Selectable(label, selected,
                              ImGuiSelectableFlags_None, ImVec2(0, 32))) {
            selectedPlatform_ = static_cast<BuildPlatform>(i);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void BuildController::drawPlatformSettings(float bottomReserve) {
    ImGui::BeginChild("##PlatformSettings", ImVec2(0, -bottomReserve),
                      ImGuiChildFlags_Borders);
    drawStartupScene();
    switch (selectedPlatform_) {
        case BuildPlatform::Windows: drawWindowsSettings(); break;
        case BuildPlatform::MetaQuest: drawQuestSettings(); break;
        case BuildPlatform::Linux: drawLinuxSettings(); break;
        case BuildPlatform::WebGL: drawWebSettings(); break;
    }
    ImGui::EndChild();
}

void BuildController::drawStartupScene() {
    ImGui::SeparatorText("Startup Scene");
    if (scenes_.empty()) {
        ImGui::TextColored(
            ImVec4(0.85f, 0.55f, 0.20f, 1.0f),
            "No .scene files found under scenes/. Save a scene first.");
    } else {
        if (mainSceneIndex_ < 0 ||
            mainSceneIndex_ >= static_cast<int>(scenes_.size()))
            mainSceneIndex_ = 0;
        ImGui::Text("Scene launched when the game starts:");
        ImGui::SetNextItemWidth(-1);
        const char* preview = scenes_[mainSceneIndex_].c_str();
        if (ImGui::BeginCombo("##MainScene", preview)) {
            for (int i = 0; i < static_cast<int>(scenes_.size()); ++i) {
                const bool selected = i == mainSceneIndex_;
                if (ImGui::Selectable(scenes_[i].c_str(), selected))
                    mainSceneIndex_ = i;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::Spacing();
    ImGui::Separator();
}

void BuildController::drawWindowsSettings() {
    ImGui::SeparatorText("Windows Build Settings");
    ImGui::Text(
        "Target Architecture: x86_64 (Direct3D 12 & Vulkan)");
    ImGui::Spacing();

    ImGui::Text("Build Configuration:");
    if (ImGui::RadioButton(
            "Debug##win", configuration_ == BuildConfig::Debug))
        configuration_ = BuildConfig::Debug;
    ImGui::SameLine();
    if (ImGui::RadioButton(
            "Release##win", configuration_ == BuildConfig::Release))
        configuration_ = BuildConfig::Release;
    ImGui::SameLine();
    if (ImGui::RadioButton(
            "Profile##win", configuration_ == BuildConfig::Profile))
        configuration_ = BuildConfig::Profile;

    ImGui::Spacing();
    ImGui::Text("Output Binary Directory:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##WinOutputPath", outputPath_,
                     sizeof(outputPath_));

    ImGui::Spacing();
    ImGui::Text("Product Version (a.b.c[.d]):");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("##WinVersion", version_, sizeof(version_));
    unsigned short parsedVersion[4];
    if (!parseExeVersion(version_, parsedVersion)) {
        ImGui::SameLine();
        ImGui::TextColored(
            ImVec4(0.90f, 0.40f, 0.40f, 1.0f),
            "invalid — expected e.g. 1.0.0");
    }

    ImGui::Text("Company (optional):");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##WinCompany", company_, sizeof(company_));
    ImGui::Text("Icon .ico (optional, project-relative):");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##WinIcon", iconPath_, sizeof(iconPath_));

    ImGui::Spacing();
    ImGui::Checkbox("Copy assets/ directory to output path",
                    &copyAssets_);
    ImGui::Checkbox("Enable Link-Time Optimization (LTO / -O3)",
                    &enableLto_);
}

void BuildController::drawQuestSettings() {
    ImGui::SeparatorText("Meta Quest (XR) Settings");
    ImGui::TextColored(
        ImVec4(0.412f, 0.714f, 0.184f, 1.0f),
        "[Meta OpenXR Core SDK v62.0]");
    ImGui::Spacing();
    ImGui::Text(
        "Graphics API: Vulkan 1.3 (Stereo Foveated Rendering)");
    ImGui::Spacing();

    static int questTarget = 2;
    ImGui::Text("Target Hardware Device:");
    ImGui::RadioButton("Quest 2", &questTarget, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Quest Pro", &questTarget, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Quest 3", &questTarget, 2);

    ImGui::Spacing();
    static bool questHandTracking = true;
    ImGui::Checkbox("High-Frequency Hand Tracking (60Hz tracking)",
                    &questHandTracking);
    static bool questFoveated = true;
    ImGui::Checkbox("Enable Foveation (Fixed/Dynamic Level High)",
                    &questFoveated);
    static bool questMultiview = true;
    ImGui::Checkbox(
        "Optimize through Mobile Multi-view (OVR_multiview2)",
        &questMultiview);

    ImGui::Spacing();
    ImGui::TextDisabled(
        "Note: Meta Quest target builds require Android NDK "
        "(Clang compiler) and Vulkan mobile SPIR-V shaders "
        "compilation pipeline.");
}

void BuildController::drawLinuxSettings() {
    ImGui::SeparatorText("Linux Build Settings");
    ImGui::Text(
        "Target Architecture: x86_64 & AArch64 (GCC/Clang)");
    ImGui::Spacing();

    ImGui::Text("Build Configuration:");
    if (ImGui::RadioButton(
            "Debug##lin", configuration_ == BuildConfig::Debug))
        configuration_ = BuildConfig::Debug;
    ImGui::SameLine();
    if (ImGui::RadioButton(
            "Release##lin", configuration_ == BuildConfig::Release))
        configuration_ = BuildConfig::Release;
    ImGui::SameLine();
    if (ImGui::RadioButton(
            "Profile##lin", configuration_ == BuildConfig::Profile))
        configuration_ = BuildConfig::Profile;

    ImGui::Spacing();
    ImGui::Checkbox(
        "Statically link GCC runtime libraries (libstdc++/libgcc)",
        &copyAssets_);
    ImGui::Checkbox(
        "Strip local debugging symbols to reduce binary size",
        &enableLto_);
}

void BuildController::drawWebSettings() {
    ImGui::SeparatorText("Web (WebGPU / WebAssembly) Settings");
    ImGui::TextColored(
        ImVec4(0.85f, 0.65f, 0.20f, 1.0f),
        "[WebGPU via Emscripten — rhi/webgpu backend]");
    ImGui::Spacing();
    ImGui::Text(
        "Compiler backend: Emscripten (wasm32) + emdawnwebgpu");
    ImGui::Spacing();
    ImGui::Text("Output Directory:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##WebOutputPath", outputPath_,
                     sizeof(outputPath_));
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Export copies the compiled game player "
        "(build-web-player/) plus the COOP/COEP dev server. "
        "Compile it first from a shell with emsdk:");
    ImGui::TextDisabled(
        "  ./web/build_web_player.sh   (then Export here)");
    ImGui::TextDisabled(
        "  python web/serve.py <out>/web  ->  "
        "http://localhost:8080");
}

void BuildController::drawResult() {
    if (hasResult_) {
        if (lastSuccess_) {
            ImGui::TextColored(
                ImVec4(0.40f, 0.80f, 0.40f, 1.0f),
                "Build succeeded: %s", lastExe_.c_str());
            if (ImGui::Button("Open Folder"))
                BuildExporter::openInExplorer(lastOutputDir_);
            ImGui::SameLine();
            if (ImGui::Button("Run Game"))
                BuildExporter::launch(lastExe_);
        } else {
            ImGui::TextColored(
                ImVec4(0.90f, 0.40f, 0.40f, 1.0f),
                "Build failed: %s", lastError_.c_str());
        }
        ImGui::BeginChild("##BuildLog", ImVec2(0, 90),
                          ImGuiChildFlags_Borders);
        ImGui::TextUnformatted(lastLog_.c_str());
        ImGui::EndChild();
        ImGui::Spacing();
    }
}

void BuildController::drawFooter(Project* project) {
    const bool isWindows =
        selectedPlatform_ == BuildPlatform::Windows;
    const bool isWeb = selectedPlatform_ == BuildPlatform::WebGL;
    const bool canBuild =
        (isWindows || isWeb) && project && project->isLoaded() &&
        !scenes_.empty();

    if (!isWindows && !isWeb)
        ImGui::TextDisabled(
            "This platform is not available yet — Windows and Web only.");

    ImGui::BeginDisabled(!canBuild);
    if (ImGui::Button("Build", ImVec2(100, 0)))
        executeBuild(project, isWeb, false);
    ImGui::SameLine();
    if (ImGui::Button("Build & Run", ImVec2(120, 0)))
        executeBuild(project, isWeb, true);
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 116.0f);
    if (ImGui::Button("Close", ImVec2(100, 0)))
        ImGui::CloseCurrentPopup();
}

} // namespace saida
