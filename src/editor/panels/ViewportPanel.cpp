#include "editor/panels/ViewportPanel.hpp"
#include "editor/EditorUI.hpp"
#include "editor/EditorApp.hpp"
#include "core/Camera.hpp"

#include <imgui.h>

namespace saida {

void ViewportPanel::draw(EditorUI* editor, Camera* camera, float dt) {
    if (!editor->showViewportOverlay_) return;

    ImGuiWindowFlags overlayFlags = ImGuiWindowFlags_NoDecoration
                                  | ImGuiWindowFlags_NoDocking
                                  | ImGuiWindowFlags_AlwaysAutoResize
                                  | ImGuiWindowFlags_NoSavedSettings
                                  | ImGuiWindowFlags_NoFocusOnAppearing
                                  | ImGuiWindowFlags_NoNav
                                  | ImGuiWindowFlags_NoMove;

    ImVec2 pos(editor->viewportPos_.x + 16.0f, editor->viewportPos_.y + 16.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.60f);

    if (ImGui::Begin("##ViewportOverlay", nullptr, overlayFlags)) {
        if (editor->app_ && editor->app_->isPlayMode())
            ImGui::TextColored(editor->settings_.lightTheme()
                ? ImVec4(0.106f, 0.310f, 0.125f, 1.0f)
                : ImVec4(0.310f, 0.706f, 0.290f, 1.0f), "PLAY MODE");
        else
            ImGui::TextColored(editor->settings_.lightTheme()
                ? ImVec4(0.106f, 0.310f, 0.125f, 1.0f)
                : ImVec4(0.725f, 0.922f, 0.063f, 1.0f), "SCENE MODE");

        const float fps = dt > 0.0f ? 1.0f / dt : 0.0f;
        ImGui::Text("%.1f FPS", fps);

        if (camera) {
            ImGui::Text("Cam: %.1f %.1f %.1f",
                camera->position.x, camera->position.y, camera->position.z);
        }
        ImGui::TextDisabled("TAB: toggle cursor");
    }
    ImGui::End();

    ImVec2 toolbarPos(editor->viewportPos_.x + 16.0f, editor->viewportPos_.y + editor->viewportSize_.y * 0.5f - 60.0f);
    ImGui::SetNextWindowPos(toolbarPos, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.60f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
    if (ImGui::Begin("##Toolbar", nullptr, overlayFlags)) {
        ImGui::PushStyleColor(ImGuiCol_Header, editor->settings_.lightTheme()
            ? ImVec4(0.710f, 0.690f, 0.643f, 1.0f)
            : ImVec4(0.412f, 0.714f, 0.184f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, editor->settings_.lightTheme()
            ? ImVec4(0.784f, 0.765f, 0.718f, 1.0f)
            : ImVec4(0.310f, 0.706f, 0.290f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, editor->settings_.lightTheme()
            ? ImVec4(0.710f, 0.690f, 0.643f, 1.0f)
            : ImVec4(0.725f, 0.922f, 0.063f, 1.0f));
        
        if (ImGui::Selectable(" T ", editor->gizmoMode_ == GizmoMode::Translate, 0, ImVec2(24, 24))) editor->gizmoMode_ = GizmoMode::Translate;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Translate (T)");
        if (ImGui::Selectable(" R ", editor->gizmoMode_ == GizmoMode::Rotate, 0, ImVec2(24, 24))) editor->gizmoMode_ = GizmoMode::Rotate;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotate (R)");
        if (ImGui::Selectable(" S ", editor->gizmoMode_ == GizmoMode::Scale, 0, ImVec2(24, 24))) editor->gizmoMode_ = GizmoMode::Scale;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scale (S)");
        
        ImGui::PopStyleColor(3);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

} // namespace saida
