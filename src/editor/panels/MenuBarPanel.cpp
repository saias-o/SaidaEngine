#include "editor/panels/MenuBarPanel.hpp"
#include "editor/EditorUI.hpp"
#include "editor/EditorApp.hpp"
#include "scene/Scene.hpp"
#include "project/Project.hpp"

#include <imgui.h>
#include <filesystem>

namespace saida {

void MenuBarPanel::draw(EditorUI* editor, Project* project, Scene* scene) {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Project...")) {
                editor->projectDialogs_.requestNewProject();
            }
            if (ImGui::MenuItem("Open Project...")) {
                editor->projectDialogs_.requestOpenProject(
                    SAIDA_PROJECT_ROOT);
            }
            bool hasProject = project && project->isLoaded();
            if (ImGui::MenuItem("Save Project", nullptr, false, hasProject)) {
                if (project) project->save();
            }
            ImGui::Separator();
            if (hasProject) {
                ImGui::TextDisabled("  Project: %s", project->name().c_str());
            } else {
                ImGui::TextDisabled("  (no project)");
            }
            ImGui::Separator();
            const bool canEdit = editor->canEdit();  // Play is read-only.
            if (ImGui::MenuItem("New Scene", nullptr, false, canEdit)) {
                if (scene) {
                    scene->clearChildren();
                    editor->selectedNode_ = nullptr;
                    editor->document_.clearCurrentPath();
                    editor->history_.clear();
                }
            }
            if (ImGui::MenuItem("Reload Scene", "F5", false, canEdit)) {
                editor->loadScene(editor->resolveScenePath(project));
            }
            if (ImGui::MenuItem("Open Scene...", nullptr, false, canEdit)) {
                editor->loadScene(editor->resolveScenePath(project));
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
                editor->saveScene(editor->resolveScenePath(project));
            }
            if (ImGui::MenuItem("Save Scene As...")) {
                std::string defaultName = "main.scene";
                if (!editor->document_.currentPath().empty()) {
                    std::filesystem::path p(editor->document_.currentPath());
                    defaultName = p.filename().string();
                }
                editor->projectDialogs_.requestSaveSceneAs(defaultName);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc"))      { editor->quitRequested_ = true; }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit")) {
            const bool canEdit = editor->canEdit();  // Play is read-only.
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canEdit && editor->history_.canUndo())) {
                editor->history_.undo(); editor->selectedNode_ = nullptr;
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canEdit && editor->history_.canRedo())) {
                editor->history_.redo(); editor->selectedNode_ = nullptr;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, editor->selectedNode_ != nullptr)) {
                editor->copySelected();
            }
            if (ImGui::MenuItem("Paste", "Ctrl+V", false,
                                canEdit && editor->document_.hasClipboard())) {
                editor->pasteClipboard();
            }
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, canEdit && editor->selectedNode_ != nullptr)) {
                editor->duplicateSelected();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Scene Tree",       nullptr, &editor->showSceneTree_);
            ImGui::MenuItem("Inspector",        nullptr, &editor->showInspector_);
            ImGui::MenuItem("File Browser",     nullptr, &editor->showFileBrowser_);
            ImGui::Separator();
            ImGui::MenuItem("Viewport Overlay", nullptr, &editor->showViewportOverlay_);
            ImGui::MenuItem("Profiler", "F3", &editor->showProfiler_);
            ImGui::MenuItem("Animation", nullptr, &editor->showAnimation_);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Scene")) {
            Node* parentNode = editor->selectedNode_ ? editor->selectedNode_ : scene;
            bool hasParent = parentNode != nullptr && editor->canEdit();  // Play is read-only.
            if (ImGui::MenuItem("Add Node", nullptr, false, hasParent)) {
                editor->nodeToCreateChildUnder_ = parentNode;
                editor->createType_ = CreateNodeType::Node;
            }
            if (ImGui::BeginMenu("Create 3D Object", hasParent)) {
                if (ImGui::MenuItem("Cube")) {
                    editor->nodeToCreateChildUnder_ = parentNode;
                    editor->createType_ = CreateNodeType::MeshNode;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Create FX", hasParent)) {
                if (ImGui::MenuItem("Particle System")) {
                    editor->nodeToCreateChildUnder_ = parentNode;
                    editor->createType_ = CreateNodeType::ParticleSystem;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Create Light", hasParent)) {
                if (ImGui::MenuItem("Directional Light")) {
                    editor->nodeToCreateChildUnder_ = parentNode;
                    editor->createType_ = CreateNodeType::DirectionalLight;
                }
                if (ImGui::MenuItem("Point Light")) {
                    editor->nodeToCreateChildUnder_ = parentNode;
                    editor->createType_ = CreateNodeType::PointLight;
                }
                if (ImGui::MenuItem("Spot Light")) {
                    editor->nodeToCreateChildUnder_ = parentNode;
                    editor->createType_ = CreateNodeType::SpotLight;
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            bool canDelete = editor->selectedNode_ != nullptr && editor->selectedNode_->parent() != nullptr && editor->canEdit();
            if (ImGui::MenuItem("Delete Selected", "Del", false, canDelete)) {
                editor->nodeToDelete_ = editor->selectedNode_;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Settings")) {
            if (ImGui::MenuItem("Settings")) {
                editor->settings_.requestOpen();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Build")) {
            if (ImGui::MenuItem("Build Project...")) {
                if (project && project->isLoaded()) {
                    editor->saveScene(editor->resolveScenePath(project));
                    editor->buildController_.requestOpen();
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Build Settings...", "Ctrl+Shift+B")) {
                editor->buildController_.requestOpen();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About SaidaEngine")) {
                editor->showAboutWindow_ = true;
            }
            ImGui::EndMenu();
        }

        {
            float contentWidth = ImGui::GetContentRegionAvail().x;
            float buttonWidth  = 160.0f;
            ImGui::SameLine(ImGui::GetCursorPosX() + contentWidth - buttonWidth);

            bool sceneActive = !editor->app_->isPlayMode();
            if (sceneActive) {
                ImGui::PushStyleColor(ImGuiCol_Button, editor->settings_.lightTheme()
                    ? ImVec4(0.710f, 0.690f, 0.643f, 1.0f)
                    : ImVec4(0.725f, 0.922f, 0.063f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, editor->settings_.lightTheme()
                    ? ImVec4(0.784f, 0.765f, 0.718f, 1.0f)
                    : ImVec4(0.816f, 0.965f, 0.357f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, editor->settings_.lightTheme()
                    ? ImVec4(0.106f, 0.310f, 0.125f, 1.0f)
                    : ImVec4(0.067f, 0.075f, 0.059f, 1.0f));
            }
            if (ImGui::Button("Scene##mode")) {
                editor->app_->setPlayMode(false);
            }
            if (sceneActive) ImGui::PopStyleColor(3);

            ImGui::SameLine();

            bool playActive = editor->app_->isPlayMode();
            if (playActive) {
                ImGui::PushStyleColor(ImGuiCol_Button, editor->settings_.lightTheme()
                    ? ImVec4(0.710f, 0.690f, 0.643f, 1.0f)
                    : ImVec4(0.310f, 0.706f, 0.290f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, editor->settings_.lightTheme()
                    ? ImVec4(0.784f, 0.765f, 0.718f, 1.0f)
                    : ImVec4(0.412f, 0.714f, 0.184f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, editor->settings_.lightTheme()
                    ? ImVec4(0.106f, 0.310f, 0.125f, 1.0f)
                    : ImVec4(0.067f, 0.075f, 0.059f, 1.0f));
            }
            if (ImGui::Button("Play##mode")) {
                editor->app_->setPlayMode(true);
            }
            if (playActive) ImGui::PopStyleColor(3);
        }

        ImGui::EndMainMenuBar();
    }
}

} // namespace saida
