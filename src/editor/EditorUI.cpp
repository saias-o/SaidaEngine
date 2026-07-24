#include "editor/EditorUI.hpp"

#include "core/Camera.hpp"
#include "core/Paths.hpp"
#include "core/Time.hpp"
#include "editor/Command.hpp"
#include "graphics/ResourceManager.hpp"
#include "project/Project.hpp"
#include "nodes/MeshNode.hpp"
#include "scene/Node.hpp"
#include "scene/Scene.hpp"

#include "editor/panels/MenuBarPanel.hpp"
#include "editor/panels/SceneHierarchyPanel.hpp"
#include "editor/panels/InspectorPanel.hpp"
#include "editor/panels/FileBrowserPanel.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/panels/ProfilerPanel.hpp"
#include "editor/panels/AnimationPanel.hpp"
#ifdef SAIDA_ENABLE_MCP
#include "mcp/McpBridge.hpp"
#endif

#include <memory>

#include "imgui.h"
#include "imgui_internal.h"

#include "editor/EditorApp.hpp"

#include <cstdlib>
#include <string>

namespace saida {

// Construction


EditorUI::EditorUI() : history_(document_) {
#ifdef SAIDA_ENABLE_MCP
    // SAIDA_MCP_PORT; default 8765. Disabled entirely by SAIDA_MCP=0.
    const char* disabled = std::getenv("SAIDA_MCP");
    if (!disabled || std::string(disabled) != "0") {
        uint16_t port = 8765;
        if (const char* p = std::getenv("SAIDA_MCP_PORT")) {
            try { port = static_cast<uint16_t>(std::stoi(p)); } catch (...) {}
        }
        mcp_ = std::make_unique<McpBridge>();
        mcp_->start(port);
    }
#endif
}

EditorUI::~EditorUI() = default;  // Scene/McpBridge complete here (unique_ptr members)


// Main draw entry point

void EditorUI::draw(EditorApp* app, Scene* scene, Camera* camera, Project* project, ResourceManager* resources, float dt) {
    beginFrameContext(app, scene, camera, project, resources);
    handleShortcuts(project);
    drawDockspace();
    drawPanels(scene, camera, project, resources, dt);
    drawModals(project);
    document_.select(selectedNode_);
}

void EditorUI::beginFrameContext(EditorApp* app, Scene* scene, Camera* camera,
                                 Project* project,
                                 ResourceManager* resources) {
    document_.bind(scene, resources);

    // A project create/load invalidates all per-project editor state: the undo
    // history references nodes from the previous scene, the clipboard holds a
    // subtree serialized against the old resources, etc. Drop them on change.
    if (project && project->version() != lastProjectVersion_) {
        lastProjectVersion_ = project->version();
        history_.clear();
        document_.clearSessionReferences();
        propEditId_ = 0;
        propEditOld_.reset();

        // Auto-load the project's entry-point scene (sets the document path).
        const std::string mainScene =
            projectDialogs_.findMainScene(project);
        if (!mainScene.empty()) loadScene(mainScene);
    }

    selectedNode_ = document_.selectedNode();
    app_ = app;
    ctxScene_ = scene;
    ctxCamera_ = camera;
    ctxProject_ = project;
    ctxResources_ = resources;

#ifdef SAIDA_ENABLE_MCP
    // Bind the document to the live scene, then service any queued MCP requests
    // on this (main) thread while all ctx pointers are valid.
    if (mcp_) {
        document_.bind(scene, resources);
        mcp_->poll(*this);
    }
#endif
}

void EditorUI::handleShortcuts(Project* project) {
    // Keyboard shortcuts (skip while typing in a text field).
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F3)) {
        showProfiler_ = !showProfiler_;
    }
    if (io.KeyCtrl && !io.WantTextInput) {
        // Copy and Save are read-only and stay available in Play; the mutating
        // shortcuts (undo/redo/paste/duplicate) are gated on canEdit().
        if (ImGui::IsKeyPressed(ImGuiKey_C)) copySelected();
        if (ImGui::IsKeyPressed(ImGuiKey_S)) {
            saveScene(resolveScenePath(project));
        }
        if (canEdit()) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z) && history_.canUndo()) { history_.undo(); selectedNode_ = nullptr; }
            if (ImGui::IsKeyPressed(ImGuiKey_Y) && history_.canRedo()) { history_.redo(); selectedNode_ = nullptr; }
            if (ImGui::IsKeyPressed(ImGuiKey_V)) pasteClipboard();
            if (ImGui::IsKeyPressed(ImGuiKey_D)) duplicateSelected();
        }
    }

    // Delete / Backspace removes the selected node (mirrors the context Delete).
    if (canEdit() && !io.WantTextInput && selectedNode_ && selectedNode_->parent()
        && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
        nodeToDelete_ = selectedNode_;
    }
}

void EditorUI::drawDockspace() {
    // Full-viewport dockspace with passthrough so the 3D render shows behind.
    // We use a fixed ID so the DockBuilder setup below targets the right node.
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoTitleBar
                               | ImGuiWindowFlags_NoCollapse
                               | ImGuiWindowFlags_NoResize
                               | ImGuiWindowFlags_NoMove
                               | ImGuiWindowFlags_NoBringToFrontOnFocus
                               | ImGuiWindowFlags_NoNavFocus
                               | ImGuiWindowFlags_NoDocking
                               | ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##DockSpaceHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspaceId = ImGui::GetID("EditorDockSpace");
    ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);

    // Build the default dock layout once.
    if (!dockLayoutBuilt_) {
        dockLayoutBuilt_ = true;

        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

        // Split: left (18%) | rest
        ImGuiID dockLeft, dockRemaining;
        ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.18f, &dockLeft, &dockRemaining);

        // Split remaining: rest | right (22%)
        ImGuiID dockRight, dockCenter;
        ImGui::DockBuilderSplitNode(dockRemaining, ImGuiDir_Right, 0.22f, &dockRight, &dockCenter);

        // Split center: viewport | bottom (25% of center height)
        ImGuiID dockBottom, dockViewport;
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, 0.25f, &dockBottom, &dockViewport);

        ImGui::DockBuilderDockWindow("Scene Tree",   dockLeft);
        ImGui::DockBuilderDockWindow("Inspector",    dockRight);
        ImGui::DockBuilderDockWindow("File Browser", dockBottom);

        ImGui::DockBuilderFinish(dockspaceId);
    }

    if (ImGuiDockNode* centralNode = ImGui::DockBuilderGetCentralNode(dockspaceId)) {
        viewportPos_ = glm::vec2(centralNode->Pos.x, centralNode->Pos.y);
        viewportSize_ = glm::vec2(centralNode->Size.x, centralNode->Size.y);
    }

    ImGui::End();  // DockSpaceHost
}

void EditorUI::drawPanels(Scene* scene, Camera* camera, Project* project,
                          ResourceManager* resources, float dt) {
    MenuBarPanel menuBarPanel;
    menuBarPanel.draw(this, project, scene);

    if (showSceneTree_) {
        SceneHierarchyPanel hierarchyPanel;
        hierarchyPanel.draw(this, scene);
    }
    
    if (showInspector_) {
        InspectorPanel inspectorPanel;
        inspectorPanel.draw(this);
    }
    
    if (showFileBrowser_) {
        FileBrowserPanel fileBrowserPanel;
        fileBrowserPanel.draw(this, project, resources);
    }

    modelImporter_.draw();

    if (showProfiler_) {
        ProfilerPanel profilerPanel;
        profilerPanel.draw(this);
    }

    if (showAnimation_) {
        AnimationPanel animationPanel;
        animationPanel.draw(this, scene, resources, project);
    }

    ViewportPanel viewportPanel;
    viewportPanel.draw(this, camera, dt);
    
    gizmo_.draw(*this, camera, scene);
    gizmo_.drawColliders(*this, camera, scene);
}

void EditorUI::drawModals(Project* project) {
    // Modal dialogs.
    drawAboutWindow();
    buildController_.draw(project);
    settings_.draw(project);
    const ProjectDialogs::Actions dialogActions =
        projectDialogs_.draw(project);
    if (!dialogActions.browseRoot.empty())
        currentBrowsePath_ = dialogActions.browseRoot;
    if (!dialogActions.sceneToSave.empty())
        saveScene(dialogActions.sceneToSave);
}

// Scene update: serialization, clipboard, undo/redo helpers

bool EditorUI::canEdit() const {
    return !(app_ && app_->isPlayMode());
}

bool EditorUI::runAutomatedBuild(Project* project, bool web,
                                 const std::string& outputDir,
                                 std::string* message) {
    return buildController_.runAutomatedBuild(
        project, web, outputDir, message);
}

void EditorUI::markDirty() {
    if (canEdit()) document_.markDirty();
}

void EditorUI::execute(std::unique_ptr<Command> command) {
    if (!canEdit()) return;  // Play mode is read-only.
    history_.execute(std::move(command));
}

void EditorUI::saveScene(const std::string& path) {
    document_.save(path);
}

void EditorUI::loadScene(const std::string& path) {
    if (document_.load(path)) {
        selectedNode_ = nullptr;
        history_.clear();
    }
}

void EditorUI::copySelected() {
    document_.copy(selectedNode_);
}

void EditorUI::pasteClipboard() {
    if (!canEdit()) return;
    Node* parent = selectedNode_ ? selectedNode_ : document_.scene();
    if (Node* pasted = document_.paste(parent, history_))
        selectedNode_ = pasted;
}

void EditorUI::duplicateSelected() {
    if (!canEdit()) return;
    if (Node* duplicate = document_.duplicate(selectedNode_, history_))
        selectedNode_ = duplicate;
}

void EditorUI::openModelImporter(const std::string& path, ResourceManager* resources) {
    modelImporter_.open(path, ctxProject_, resources);
}

// About SaidaEngine dialog (modal popup)

void EditorUI::drawAboutWindow() {
    if (showAboutWindow_) {
        ImGui::OpenPopup("About SaidaEngine");
        showAboutWindow_ = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("About SaidaEngine", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Spacing();

        brandingImages_.beginFrame();
        ImTextureID logo = 0;
        if (ctxResources_)
            logo = brandingImages_.get(ctxResources_->device(), assetPath("assets/editor/saida_logo.png"));

        if (logo) {
            constexpr float logoWidth = 96.0f;
            constexpr float logoHeight = logoWidth * 306.0f / 256.0f;
            const float availableWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availableWidth - logoWidth) * 0.5f);
            ImGui::Image(logo, ImVec2(logoWidth, logoHeight));
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text(" SaidaEngine C++ Game Engine");
        ImGui::Text(" Version: "); ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.30f, 0.70f, 0.35f, 1.0f), "Alpha 0.0.1"); // Sleek Green

        ImGui::Spacing();
        ImGui::Text(" A lightweight, Vulkan-powered 3D editor and runtime.");
        ImGui::Text(" Designed for modularity, speed, and simplicity.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextDisabled(" Powered by Vulkan, GLFW, Dear ImGui, and GLM.");
        ImGui::TextDisabled(" Copyright (c) 2026 SaidaEngine Contributors.");

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.851f, 0.576f, 0.290f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.951f, 0.676f, 0.390f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.751f, 0.476f, 0.190f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        
        if (ImGui::Button("Sponsor SaidaEngine (Donate)", ImVec2(ImGui::GetContentRegionAvail().x, 35.0f))) {
#ifdef _WIN32
            std::system("start https://github.com/sponsors/saias-o");
#elif __APPLE__
            std::system("open https://github.com/sponsors/saias-o");
#else
            std::system("xdg-open https://github.com/sponsors/saias-o");
#endif
        }
        ImGui::PopStyleColor(4);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Close button aligned nicely
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 116.0f);
        if (ImGui::Button("Close", ImVec2(100, 0))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

std::string EditorUI::resolveScenePath(Project* project) const {
    return projectDialogs_.resolveScenePath(
        document_.currentPath(), project);
}

} // namespace saida
