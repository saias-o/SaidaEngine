#include "editor/ProjectDialogs.hpp"

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "project/Project.hpp"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace saida {

namespace {

bool isPrunedScanDir(const std::string& name) {
    return name == "build" || name == "third_party" || name == ".git" ||
           name == "node_modules" || (!name.empty() && name[0] == '.');
}

std::vector<std::string> scanProjectFiles(std::string root) {
    namespace fs = std::filesystem;
    std::vector<std::string> projects;
    std::error_code error;

    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, error);
    const fs::recursive_directory_iterator end;
    for (; !error && it != end; it.increment(error)) {
        const fs::path& path = it->path();
        if (it->is_directory(error)) {
            if (isPrunedScanDir(path.filename().string()))
                it.disable_recursion_pending();
            continue;
        }
        if (path.extension() != ".saidaproj") continue;
        const std::string name = path.filename().string();
        if (!name.empty() && name[0] != '.')
            projects.push_back(path.string());
    }
    std::sort(projects.begin(), projects.end());
    return projects;
}

} // namespace

ProjectDialogs::ProjectDialogs()
    : browseRoot_(SAIDA_PROJECT_ROOT) {
    std::strncpy(newProjectPath_, SAIDA_PROJECT_ROOT,
                 sizeof(newProjectPath_) - 1);
    newProjectPath_[sizeof(newProjectPath_) - 1] = '\0';
    startScan(browseRoot_);
}

void ProjectDialogs::requestOpenProject(const std::string& root) {
    browseRoot_ = root;
    openProjectRequested_ = true;
}

void ProjectDialogs::requestSaveSceneAs(const std::string& defaultName) {
    std::snprintf(saveScenePath_, sizeof(saveScenePath_), "%s",
                  defaultName.c_str());
    openSaveAsRequested_ = true;
}

void ProjectDialogs::startScan(const std::string& root) {
    scanFuture_ =
        std::async(std::launch::async, scanProjectFiles, root);
}

ProjectDialogs::Actions ProjectDialogs::draw(Project* project) {
    Actions actions;
    drawNewProject(project, actions);
    drawOpenProject(project, actions);
    drawSaveSceneAs(project, actions);
    return actions;
}

void ProjectDialogs::drawNewProject(Project* project, Actions& actions) {
    if (openNewRequested_) {
        ImGui::OpenPopup("New Project");
        openNewRequested_ = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500, 0), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal(
            "New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::Text("Create a new SaidaEngine project.");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("Project Name:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##projname", newProjectName_,
                     sizeof(newProjectName_));
    ImGui::Spacing();
    ImGui::Text("Parent Directory:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##projpath", newProjectPath_,
                     sizeof(newProjectPath_));
    ImGui::Spacing();

    namespace fs = std::filesystem;
    const fs::path fullPath =
        fs::path(newProjectPath_) / newProjectName_;
    ImGui::TextDisabled("Will create: %s", fullPath.string().c_str());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const bool nameValid = std::strlen(newProjectName_) > 0;
    const bool pathValid =
        std::strlen(newProjectPath_) > 0 &&
        fs::is_directory(newProjectPath_);
    if (!pathValid && std::strlen(newProjectPath_) > 0) {
        ImGui::TextColored(
            ImVec4(1, 0.4f, 0.4f, 1),
            "Parent directory does not exist.");
    }

    if (ImGui::Button("Create", ImVec2(120, 0)) &&
        nameValid && pathValid) {
        if (project) {
            project->create(newProjectPath_, newProjectName_);
            actions.browseRoot = project->rootPath();
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0)))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

void ProjectDialogs::drawOpenProject(Project* project, Actions& actions) {
    if (openProjectRequested_) {
        ImGui::OpenPopup("Open Project");
        openProjectRequested_ = false;
        if (!scanFuture_.valid()) startScan(browseRoot_);
    }

    if (scanFuture_.valid() &&
        scanFuture_.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
        projectCache_ = scanFuture_.get();
        initialScanDone_ = true;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(620, 460),
                            ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal(
            "Open Project", nullptr, ImGuiWindowFlags_None))
        return;

    ImGui::Text("Search root:");
    ImGui::SameLine();
    char rootBuffer[512];
    std::strncpy(rootBuffer, browseRoot_.c_str(),
                 sizeof(rootBuffer) - 1);
    rootBuffer[sizeof(rootBuffer) - 1] = '\0';
    ImGui::SetNextItemWidth(-80.0f);

    bool rescan = false;
    if (ImGui::InputText("##RootPath", rootBuffer,
                         sizeof(rootBuffer),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        browseRoot_ = rootBuffer;
        rescan = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Scan")) {
        browseRoot_ = rootBuffer;
        rescan = true;
    }
    if (rescan) startScan(browseRoot_);

    drawOpenProjectResults(project, actions, scanFuture_.valid());
    if (ImGui::Button("Cancel", ImVec2(120, 0)))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void ProjectDialogs::drawOpenProjectResults(Project* project,
                                            Actions& actions,
                                            bool scanning) {
    namespace fs = std::filesystem;
    ImGui::Separator();
    if (!initialScanDone_ && scanning) {
        ImGui::TextDisabled("Scanning for projects…");
    } else if (projectCache_.empty()) {
        ImGui::TextDisabled(
            "No .saidaproj files found under this directory.");
    } else {
        ImGui::Text("%zu project(s) found — double-click to open:%s",
                    projectCache_.size(),
                    scanning ? "  (refreshing…)" : "");
    }

    ImGui::BeginChild(
        "##OpenBrowse",
        ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 4),
        ImGuiChildFlags_Borders);
    for (const std::string& projectPath : projectCache_) {
        std::string label;
        try {
            label = fs::relative(projectPath, browseRoot_).string();
        } catch (...) {
            label = projectPath;
        }
        std::replace(label.begin(), label.end(), '\\', '/');

        if (ImGui::Selectable(
                label.c_str(), false,
                ImGuiSelectableFlags_AllowDoubleClick) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (project) {
                project->load(projectPath);
                actions.browseRoot = project->rootPath();
            }
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", projectPath.c_str());
    }
    ImGui::EndChild();
}

void ProjectDialogs::drawSaveSceneAs(Project* project,
                                     Actions& actions) {
    if (openSaveAsRequested_) {
        ImGui::OpenPopup("Save Scene As");
        openSaveAsRequested_ = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 150),
                            ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal(
            "Save Scene As", nullptr,
            ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoSavedSettings))
        return;

    ImGui::Text("Enter scene file name (e.g. level1.scene):");
    ImGui::Spacing();
    ImGui::InputText("##SceneName", saveScenePath_,
                     sizeof(saveScenePath_));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Save", ImVec2(120, 0))) {
        std::string fileName = saveScenePath_;
        if (fileName.find(".scene") == std::string::npos &&
            !fileName.empty())
            fileName += ".scene";

        if (!fileName.empty()) {
            actions.sceneToSave =
                project && project->isLoaded()
                ? project->scenesDir() + "/" + fileName
                : fileName;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0)))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

std::string ProjectDialogs::findMainScene(Project* project) const {
    if (!project) return {};

    namespace fs = std::filesystem;
    if (!project->mainScene().empty()) {
        const std::string declared =
            project->rootPath() + "/" + project->mainScene();
        if (fs::exists(declared)) return declared;
        Log::warn("Project '", project->name(),
                  "': declared main_scene '", project->mainScene(),
                  "' not found, falling back.");
    }

    const std::string conventional =
        project->scenesDir() + "/main.scene";
    if (fs::exists(conventional)) return conventional;

    const fs::path scenesDir(project->scenesDir());
    if (fs::exists(scenesDir)) {
        std::vector<fs::path> found;
        try {
            for (const auto& entry :
                 fs::recursive_directory_iterator(
                     scenesDir,
                     fs::directory_options::skip_permission_denied)) {
                if (entry.path().extension() == ".scene")
                    found.push_back(entry.path());
            }
        } catch (const fs::filesystem_error& error) {
            Log::warn("findMainScene: scene scan failed: ",
                      error.what());
        }
        std::sort(found.begin(), found.end());
        if (!found.empty()) return found.front().string();
    }

    Log::info("Project '", project->name(),
              "': no scene found, starting with empty scene.");
    return {};
}

std::string ProjectDialogs::resolveScenePath(
    const std::string& currentPath, Project* project) const {
    if (!currentPath.empty()) return currentPath;
    if (project && project->isLoaded())
        return project->scenesDir() + "/main.scene";
    return "scene.scene";
}

} // namespace saida
