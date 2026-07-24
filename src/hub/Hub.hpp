#pragma once

#include "core/Window.hpp"
#include "graphics/VulkanDevice.hpp"
#include "graphics/Swapchain.hpp"
#include "graphics/ImGuiLayer.hpp"
#include "project/Project.hpp"

#include <memory>
#include <string>
#include <vector>

namespace saida {

class Hub {
public:
    Hub();
    ~Hub();
    Hub(const Hub&) = delete;
    Hub& operator=(const Hub&) = delete;

    void run();

private:
    void setupStyle();
    void loadProjects();
    void saveProjects();
    void drawUI();
    void launchProject(const std::string& path);
    void launchTemplate();
    
    std::unique_ptr<Window> window_;
    std::unique_ptr<VulkanDevice> device_;
    std::unique_ptr<Swapchain> swapchain_;
    std::unique_ptr<ImGuiLayer> imgui_;

    struct ProjectEntry {
        std::string name;
        std::string path;
    };
    std::vector<ProjectEntry> projects_;

    std::string hubJsonPath_;
    bool shouldClose_ = false;

    // UI state
    int currentTab_ = 0; // 0 = Projects, 1 = Templates/New
    char newProjectName_[128] = "MyProject";
    char newProjectPath_[512] = "C:/Projects/";

    int renameIndex_ = -1;
    char renameBuf_[128] = "";
};

} // namespace saida
