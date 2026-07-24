#include "Hub.hpp"
#include "core/Log.hpp"
#include "graphics/GpuSync.hpp"
#include "project/ProjectRename.hpp"
#include "imgui.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <algorithm>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace saida {

Hub::Hub() {
    window_ = std::make_unique<Window>(1000, 600, "SaidaEngine Hub");
    device_ = std::make_unique<VulkanDevice>(*window_);
    swapchain_ = std::make_unique<Swapchain>(*device_, *window_);
    imgui_ = std::make_unique<ImGuiLayer>(*device_, *window_, swapchain_->colorFormat(),
                                          swapchain_->imageCount(), swapchain_->samples());

    const char* appData = std::getenv("APPDATA");
    if (appData) {
        fs::path dir = fs::path(appData) / "SaidaEngine";
        if (!fs::exists(dir)) {
            fs::create_directories(dir);
        }
        hubJsonPath_ = (dir / "hub.json").string();
    } else {
        hubJsonPath_ = "hub.json";
    }

    setupStyle();
    loadProjects();
}

Hub::~Hub() {
    vkDeviceWaitIdle(device_->device());
}

void Hub::setupStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    
    style.WindowPadding     = ImVec2(15, 15);
    style.WindowRounding    = 8.0f;
    style.FramePadding      = ImVec2(12, 8);
    style.FrameRounding     = 6.0f;
    style.ItemSpacing       = ImVec2(12, 12);
    style.ItemInnerSpacing  = ImVec2(8, 6);
    style.ScrollbarSize     = 12.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabMinSize       = 12.0f;
    style.GrabRounding      = 6.0f;
    style.ChildRounding     = 8.0f;
    style.WindowBorderSize  = 0.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 0.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                   = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.12f, 0.13f, 0.15f, 1.00f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.12f, 0.13f, 0.15f, 1.00f);
    colors[ImGuiCol_Border]                 = ImVec4(0.18f, 0.20f, 0.22f, 1.00f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    
    colors[ImGuiCol_FrameBg]                = ImVec4(0.15f, 0.17f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.20f, 0.22f, 0.25f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.25f, 0.27f, 0.30f, 1.00f);
    
    colors[ImGuiCol_TitleBg]                = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.18f, 0.20f, 0.22f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.25f, 0.27f, 0.30f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.35f, 0.37f, 0.40f, 1.00f);
    
    colors[ImGuiCol_Button]                 = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.24f, 0.26f, 0.30f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.30f, 0.32f, 0.36f, 1.00f);
    
    colors[ImGuiCol_Header]                 = ImVec4(0.20f, 0.24f, 0.32f, 1.00f); 
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.25f, 0.30f, 0.38f, 1.00f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.30f, 0.35f, 0.45f, 1.00f);
    
    colors[ImGuiCol_Separator]              = ImVec4(0.18f, 0.20f, 0.22f, 1.00f);
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.30f, 0.32f, 0.35f, 1.00f);
    colors[ImGuiCol_SeparatorActive]        = ImVec4(0.40f, 0.60f, 0.95f, 1.00f);
}

void Hub::loadProjects() {
    if (!fs::exists(hubJsonPath_)) return;

    try {
        std::ifstream f(hubJsonPath_);
        json j;
        f >> j;
        for (const auto& item : j["projects"]) {
            projects_.push_back({
                item.value("name", "Unknown"),
                item.value("path", "")
            });
        }
    } catch (const std::exception& e) {
        Log::warn("Failed to load hub.json: " + std::string(e.what()));
    }
}

void Hub::saveProjects() {
    try {
        json j;
        j["projects"] = json::array();
        for (const auto& p : projects_) {
            j["projects"].push_back({
                {"name", p.name},
                {"path", p.path}
            });
        }
        std::ofstream f(hubJsonPath_);
        f << j.dump(4);
    } catch (const std::exception& e) {
        Log::warn("Failed to save hub.json: " + std::string(e.what()));
    }
}

void Hub::launchProject(const std::string& path) {
    fs::path exePath = fs::current_path() / "build" / "bin" / "SaidaEngine.exe";
    if (!fs::exists(exePath)) exePath = fs::current_path() / "bin" / "SaidaEngine.exe";
    if (!fs::exists(exePath)) exePath = fs::current_path() / "SaidaEngine.exe";
    
    std::string cmd = "start \"\" \"" + exePath.string() + "\" --project \"" + path + "\"";
    std::system(cmd.c_str());
    shouldClose_ = true;
}

void Hub::launchTemplate() {
    fs::path exePath = fs::current_path() / "build" / "bin" / "SaidaEngine.exe";
    if (!fs::exists(exePath)) exePath = fs::current_path() / "bin" / "SaidaEngine.exe";
    if (!fs::exists(exePath)) exePath = fs::current_path() / "SaidaEngine.exe";
    std::string cmd = "start \"\" \"" + exePath.string() + "\"";
    std::system(cmd.c_str());
    shouldClose_ = true;
}

void Hub::drawUI() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("SaidaEngine Hub", nullptr, flags);
    ImGui::PopStyleVar();

    // Sidebar
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.09f, 0.11f, 1.0f));
    ImGui::BeginChild("Sidebar", ImVec2(220, 0), false);
    
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Indent(20.0f);
    
    ImGui::SetWindowFontScale(1.4f);
    ImGui::TextColored(ImVec4(0.4f, 0.6f, 0.95f, 1.0f), "SaidaEngine");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("Hub v1.0");
    
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
    if (ImGui::Selectable("  Projects", currentTab_ == 0, 0, ImVec2(180, 40))) currentTab_ = 0;
    ImGui::Spacing();
    if (ImGui::Selectable("  Installs / Templates", currentTab_ == 1, 0, ImVec2(180, 40))) currentTab_ = 1;
    ImGui::PopStyleVar();

    ImGui::Unindent(20.0f);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    
    ImGui::SameLine();

    // Content area
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.13f, 0.15f, 1.0f));
    ImGui::BeginChild("Content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    
    ImGui::Spacing();
    ImGui::Spacing();
    
    if (currentTab_ == 0) {
        ImGui::SetWindowFontScale(1.8f);
        ImGui::Text("Projects");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Spacing();

        if (ImGui::Button("  + New Project...  ", ImVec2(0, 40))) {
            currentTab_ = 1;
        }
        
        ImGui::Spacing();
        ImGui::Spacing();

        if (projects_.empty()) {
            ImGui::TextDisabled("No projects found. Create one to get started!");
        } else {
            for (size_t i = 0; i < projects_.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                
                if (renameIndex_ == static_cast<int>(i)) {
                    ImGui::InputText("##rename", renameBuf_, sizeof(renameBuf_));
                    ImGui::SameLine();
                    if (ImGui::Button("Save")) {
                        // Directory, .saidaproj (file name + name field) and
                        // the hub.json entry move together, with rollback on
                        // failure; the entry is only updated on success and
                        // the rename box stays open on refusal.
                        const ProjectRenameResult renamed = renameProjectDirectory(
                            projects_[i].path, renameBuf_, hubJsonPath_);
                        if (renamed) {
                            projects_[i].name = renameBuf_;
                            projects_[i].path = renamed.rootPath;
                            renameIndex_ = -1;
                        } else {
                            Log::warn("Project rename refused: " + renamed.error);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) {
                        renameIndex_ = -1;
                    }
                } else {
                    float rowHeight = ImGui::GetTextLineHeightWithSpacing() * 2;
                    ImVec2 startPos = ImGui::GetCursorPos();
                    
                    // Render an invisible Selectable spanning the row
                    if (ImGui::Selectable(std::string("##row" + std::to_string(i)).c_str(), false, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowHeight))) {
                        launchProject(projects_[i].path);
                    }
                    
                    // Render the text over the selectable
                    ImGui::SetCursorPos(startPos);
                    ImGui::BeginGroup();
                    ImGui::Text("%s", projects_[i].name.c_str());
                    ImGui::TextDisabled("%s", projects_[i].path.c_str());
                    ImGui::EndGroup();
                    
                    // Render buttons on the right side
                    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 120, startPos.y + ImGui::GetStyle().ItemSpacing.y));
                    if (ImGui::Button("Rename")) {
                        renameIndex_ = static_cast<int>(i);
                        std::strncpy(renameBuf_, projects_[i].name.c_str(), sizeof(renameBuf_) - 1);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("X")) {
                        std::error_code ec;
                        fs::remove_all(projects_[i].path, ec);
                        if (ec) {
                            Log::warn("Failed to delete project directory: " + ec.message());
                        }
                        projects_.erase(projects_.begin() + i);
                        saveProjects();
                        ImGui::PopID();
                        break;
                    }
                    
                    ImGui::SetCursorPosY(startPos.y + rowHeight);
                }

                ImGui::Separator();
                ImGui::PopID();
            }
        }
    } else if (currentTab_ == 1) {
        ImGui::SetWindowFontScale(1.8f);
        ImGui::Text("Create New Project");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::Text("Project Details");
        ImGui::InputText("Project Name", newProjectName_, sizeof(newProjectName_));
        ImGui::Spacing();
        ImGui::InputText("Parent Directory", newProjectPath_, sizeof(newProjectPath_));
        ImGui::Spacing();
        ImGui::Spacing();
        
        if (ImGui::Button("  Create Project  ", ImVec2(0, 40))) {
            Project p;
            if (p.create(newProjectPath_, newProjectName_)) {
                projects_.push_back({newProjectName_, p.rootPath()});
                saveProjects();
                currentTab_ = 0;
            } else {
                Log::error("Failed to create project!");
            }
        }

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::SetWindowFontScale(1.8f);
        ImGui::Text("Templates");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Spacing();
        
        if (ImGui::Button("  Launch Default Demo Scene  ", ImVec2(0, 40))) {
            launchTemplate();
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(); // Pop Content bg
    ImGui::End();
}

void Hub::run() {
    // The Hub is single-frame-in-flight, so it always uses slot 0.
    VkCommandPool pool;
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = device_->findQueueFamilies().graphicsFamily.value();
    vkCreateCommandPool(device_->device(), &poolInfo, nullptr, &pool);

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_->device(), &allocInfo, &cmd);

    while (!window_->shouldClose() && !shouldClose_) {
        window_->pollEvents();
        
        int w, h;
        window_->framebufferSize(w, h);
        if (w == 0 || h == 0) {
            continue;
        }

        swapchain_->waitFrame(0);

        uint32_t imageIndex;
        if (!swapchain_->acquire(0, imageIndex)) {
            continue;  // out of date: the surface recreated itself
        }

        vkResetCommandBuffer(cmd, 0);

        imgui_->beginFrame();
        drawUI();
        imgui_->endFrame();

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &beginInfo);
        
        const bool msaa = swapchain_->samples() != VK_SAMPLE_COUNT_1_BIT;
        VkImage swapImage = swapchain_->image(imageIndex);
        VkImage colorImage = msaa ? swapchain_->msaaColorImage() : swapImage;
        VkImageView colorView = msaa ? swapchain_->msaaColorView() : swapchain_->imageView(imageIndex);

        std::array<VkImageMemoryBarrier2, 2> pre{};
        uint32_t preCount = 0;
        pre[preCount++] = imageBarrier2(colorImage,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        if (msaa)
            pre[preCount++] = imageBarrier2(swapImage,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        cmdImageBarriers(cmd, pre.data(), preCount);

        VkRenderingAttachmentInfo colorAttach{};
        colorAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttach.imageView = colorView;
        colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttach.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        colorAttach.clearValue.color = {{0.15f, 0.15f, 0.15f, 1.0f}};
        if (msaa) {
            colorAttach.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            colorAttach.resolveImageView = swapchain_->imageView(imageIndex);
            colorAttach.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea.extent = swapchain_->extent();
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttach;

        vkCmdBeginRendering(cmd, &renderingInfo);
        imgui_->renderDrawData(cmd);
        vkCmdEndRendering(cmd);

        VkImageMemoryBarrier2 toPresent = imageBarrier2(swapImage,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
        cmdImageBarrier(cmd, toPresent);
        vkEndCommandBuffer(cmd);

        swapchain_->submitAndPresent(cmd, 0, imageIndex);
    }

    vkDeviceWaitIdle(device_->device());
    vkDestroyCommandPool(device_->device(), pool, nullptr);
}

} // namespace saida
