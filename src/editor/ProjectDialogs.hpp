#pragma once

#include <future>
#include <string>
#include <vector>

namespace saida {

class Project;

// Owns the three project/document modal lifetimes and the asynchronous project
// scan. draw() reports actions instead of reaching into EditorUI.
class ProjectDialogs {
public:
    struct Actions {
        std::string browseRoot;
        std::string sceneToSave;
    };

    ProjectDialogs();

    void requestNewProject() { openNewRequested_ = true; }
    void requestOpenProject(const std::string& root);
    void requestSaveSceneAs(const std::string& defaultName);

    Actions draw(Project* project);

    std::string findMainScene(Project* project) const;
    std::string resolveScenePath(const std::string& currentPath,
                                 Project* project) const;

private:
    void startScan(const std::string& root);
    void drawNewProject(Project* project, Actions& actions);
    void drawOpenProject(Project* project, Actions& actions);
    void drawOpenProjectResults(Project* project, Actions& actions,
                                bool scanning);
    void drawSaveSceneAs(Project* project, Actions& actions);

    bool openNewRequested_ = false;
    bool openProjectRequested_ = false;
    bool openSaveAsRequested_ = false;
    char newProjectName_[128] = "MyGame";
    char newProjectPath_[512] = "";
    char saveScenePath_[512] = "main.scene";

    std::string browseRoot_;
    std::vector<std::string> projectCache_;
    std::future<std::vector<std::string>> scanFuture_;
    bool initialScanDone_ = false;
};

} // namespace saida
