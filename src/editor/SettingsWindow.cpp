#include "editor/SettingsWindow.hpp"

#include "audio/AudioManager.hpp"
#include "core/Paths.hpp"
#include "project/Project.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace saida {

namespace {
std::filesystem::path editorThemePreferencePath() {
    if (const char* appData = std::getenv("APPDATA")) {
        if (*appData != '\0')
            return std::filesystem::path(appData) / "SaidaEngine" / "editor_theme.txt";
    }
    return std::filesystem::path(SAIDA_PROJECT_ROOT) / "build" / "editor_theme.txt";
}

bool loadLightThemePreference() {
    std::ifstream input(editorThemePreferencePath());
    std::string value;
    return input && std::getline(input, value) && value == "light";
}

void saveLightThemePreference(bool useLightTheme) {
    const std::filesystem::path path = editorThemePreferencePath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output(path);
    if (output) output << (useLightTheme ? "light" : "dark");
}
} // namespace

SettingsWindow::SettingsWindow()
    : lightTheme_(loadLightThemePreference()) {
    applyStyle();
}

void SettingsWindow::applyStyle() const {
    applyLayout();
    applyColors();
}

void SettingsWindow::applyLayout() const {
    ImGuiStyle& style = ImGui::GetStyle();

    // The web platform uses soft 8 px cards and a deliberately relaxed rhythm.
    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 8.0f;
    style.FrameRounding     = 6.0f;
    style.PopupRounding     = 8.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding      = 6.0f;
    style.TabRounding       = 6.0f;

    // This spacing also makes context menus and the top menu less cramped.
    style.WindowPadding    = ImVec2(13, 13);
    style.FramePadding     = ImVec2(10, 7);
    style.CellPadding      = ImVec2(10, 7);
    style.ItemSpacing      = ImVec2(10, 8);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.IndentSpacing    = 22.0f;
    style.ScrollbarSize    = 13.0f;
    style.GrabMinSize      = 11.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize  = 1.0f;
    style.PopupBorderSize  = 1.0f;
    style.FrameBorderSize  = 1.0f;
    style.TabBorderSize    = 0.0f;
}

void SettingsWindow::applyColors() const {
    ImGuiStyle& style = ImGui::GetStyle();
    // Two Saida themes: ink-dark by default, paper-light as an option.
    ImVec4* c = style.Colors;
    const bool light = lightTheme_;

    // Shared Saida brand colors from the web platform.
    const ImVec4 brand     = ImVec4(0.725f, 0.922f, 0.063f, 1.0f); // #b9eb10
    const ImVec4 brandDeep = ImVec4(0.412f, 0.714f, 0.184f, 1.0f); // #69b62f
    const ImVec4 paper     = ImVec4(0.984f, 0.973f, 0.929f, 1.0f); // #fbf8ed
    const ImVec4 darkGreen = ImVec4(0.106f, 0.310f, 0.125f, 1.0f);
    const ImVec4 ink       = ImVec4(0.071f, 0.078f, 0.055f, 1.0f); // #12140e

    // Dark remains the default: neutral charcoal with a very subtle green
    // cast, so the Saida lime is the only strong hue in the editor chrome.
    const ImVec4 bg       = light ? ImVec4(0.886f, 0.871f, 0.827f, 1.0f) : ImVec4(0.063f, 0.075f, 0.067f, 1.0f);
    const ImVec4 bgChild  = light ? ImVec4(0.839f, 0.816f, 0.761f, 1.0f) : ImVec4(0.090f, 0.110f, 0.094f, 1.0f);
    const ImVec4 bgPopup  = light ? ImVec4(0.933f, 0.918f, 0.882f, 1.0f) : ImVec4(0.106f, 0.129f, 0.110f, 1.0f);
    const ImVec4 accent   = light ? darkGreen : brand;
    const ImVec4 accentH  = light ? ImVec4(0.153f, 0.396f, 0.173f, 1.0f) : ImVec4(0.816f, 0.965f, 0.357f, 1.0f);
    const ImVec4 accentA  = light ? ImVec4(0.710f, 0.690f, 0.643f, 1.0f) : brandDeep;
    // Light runs black on paper: the Saida green is a background hue here, never
    // a text hue, so panel labels keep a full-contrast ink instead of tinted type.
    const ImVec4 text     = light ? ink : paper;
    const ImVec4 textDim  = light ? ImVec4(0.318f, 0.333f, 0.290f, 1.0f) : ImVec4(0.635f, 0.702f, 0.651f, 1.0f);
    const ImVec4 border   = light ? ImVec4(0.224f, 0.243f, 0.216f, 0.78f) : ImVec4(0.208f, 0.255f, 0.224f, 0.88f);
    const ImVec4 header   = light ? ImVec4(0.855f, 0.831f, 0.776f, 1.0f) : ImVec4(0.133f, 0.165f, 0.141f, 1.0f);
    const ImVec4 headerH  = light ? ImVec4(0.784f, 0.765f, 0.718f, 1.0f) : ImVec4(0.173f, 0.220f, 0.180f, 1.0f);

    // Panel tabs ("Inspector", "File Browser", …) carry the green in light mode.
    // ImGui has no per-tab text color, so the shared ink text lands on every
    // shade below: 4.5:1 against black is the floor that decides how dark the
    // ramp can go. Selected is the saturated green, idle the muted olive, and
    // dimmed recedes by desaturating rather than by darkening further.
    const ImVec4 tab       = light ? ImVec4(0.392f, 0.522f, 0.271f, 1.0f) : header;   // #648545, 5.0:1
    const ImVec4 tabH      = light ? ImVec4(0.380f, 0.659f, 0.153f, 1.0f) : headerH;  // #61a827, 7.1:1
    const ImVec4 tabSel    = light ? ImVec4(0.310f, 0.561f, 0.118f, 1.0f) : header;   // #4f8f1e, 5.3:1
    const ImVec4 tabDim    = light ? ImVec4(0.541f, 0.580f, 0.471f, 1.0f) : bgChild;  // #8a9478, 6.6:1
    const ImVec4 tabDimSel = light ? ImVec4(0.427f, 0.580f, 0.251f, 1.0f) : header;   // #6d9440, 6.0:1
    const ImVec4 tabLine   = light ? darkGreen : brand;

    c[ImGuiCol_Text]                  = text;
    c[ImGuiCol_TextDisabled]          = textDim;
    c[ImGuiCol_WindowBg]              = bg;
    c[ImGuiCol_ChildBg]               = bgChild;
    c[ImGuiCol_PopupBg]               = bgPopup;
    c[ImGuiCol_Border]                = border;
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]               = light ? ImVec4(0.941f, 0.925f, 0.890f, 1.0f) : ImVec4(0.047f, 0.063f, 0.051f, 1.0f);
    c[ImGuiCol_FrameBgHovered]        = light ? headerH : ImVec4(0.122f, 0.165f, 0.133f, 1.0f);
    c[ImGuiCol_FrameBgActive]         = light ? bg : ImVec4(0.153f, 0.200f, 0.161f, 1.0f);
    // A floating panel's title bar is the same label chip as its docked tab.
    c[ImGuiCol_TitleBg]               = light ? tabDim : bgChild;
    c[ImGuiCol_TitleBgActive]         = light ? tabSel : ImVec4(0.110f, 0.149f, 0.118f, 1.0f);
    c[ImGuiCol_TitleBgCollapsed]      = light ? tabDim : bgChild;
    c[ImGuiCol_MenuBarBg]             = light ? bg : ImVec4(0.082f, 0.102f, 0.086f, 1.0f);
    c[ImGuiCol_ScrollbarBg]           = bgChild;
    c[ImGuiCol_ScrollbarGrab]         = light ? ImVec4(0.596f, 0.576f, 0.529f, 1.0f) : ImVec4(0.208f, 0.255f, 0.224f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered]  = light ? ImVec4(0.431f, 0.424f, 0.392f, 1.0f) : ImVec4(0.306f, 0.373f, 0.322f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive]   = accent;
    c[ImGuiCol_CheckMark]             = accent;
    c[ImGuiCol_SliderGrab]            = accent;
    c[ImGuiCol_SliderGrabActive]      = accentH;
    c[ImGuiCol_Button]                = header;
    c[ImGuiCol_ButtonHovered]         = headerH;
    c[ImGuiCol_ButtonActive]          = accentA;
    c[ImGuiCol_Header]                = header;
    c[ImGuiCol_HeaderHovered]         = headerH;
    c[ImGuiCol_HeaderActive]          = accentA;
    c[ImGuiCol_Separator]             = border;
    c[ImGuiCol_SeparatorHovered]      = accent;
    c[ImGuiCol_SeparatorActive]       = accentH;
    c[ImGuiCol_ResizeGrip]            = light ? ImVec4(0.596f, 0.576f, 0.529f, 1.0f) : border;
    c[ImGuiCol_ResizeGripHovered]     = accent;
    c[ImGuiCol_ResizeGripActive]      = accentH;
    c[ImGuiCol_Tab]                   = tab;
    c[ImGuiCol_TabHovered]            = tabH;
    c[ImGuiCol_TabSelected]           = tabSel;
    c[ImGuiCol_TabSelectedOverline]   = tabLine;
    c[ImGuiCol_TabDimmed]             = tabDim;
    c[ImGuiCol_TabDimmedSelected]     = tabDimSel;
    c[ImGuiCol_DockingPreview]        = ImVec4(accent.x, accent.y, accent.z, 0.7f);
    c[ImGuiCol_DockingEmptyBg]        = bgChild;
    c[ImGuiCol_PlotLines]             = textDim;
    c[ImGuiCol_PlotLinesHovered]      = accentH;
    c[ImGuiCol_PlotHistogram]         = accent;
    c[ImGuiCol_PlotHistogramHovered]  = accentH;
    c[ImGuiCol_TableHeaderBg]         = header;
    c[ImGuiCol_TableBorderStrong]     = border;
    c[ImGuiCol_TableBorderLight]      = ImVec4(border.x, border.y, border.z, 0.55f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]         = light ? ImVec4(0.710f, 0.690f, 0.643f, 0.28f) : ImVec4(0.412f, 0.714f, 0.184f, 0.07f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_DragDropTarget]        = accent;
    c[ImGuiCol_NavHighlight]          = accent;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(accent.x, accent.y, accent.z, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = light ? ImVec4(0, 0, 0, 0.18f) : ImVec4(0, 0, 0, 0.48f);
    c[ImGuiCol_ModalWindowDimBg]      = light ? ImVec4(0, 0, 0, 0.24f) : ImVec4(0, 0, 0, 0.58f);
}

void SettingsWindow::draw(Project* project) {
    if (!open_) return;

    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
    const bool visible = ImGui::Begin("Settings", &open_);
    if (visible) {
        drawAppearance();
        if (!project || !project->isLoaded()) {
            ImGui::TextDisabled("No project loaded.");
        } else if (ImGui::BeginTabBar("SettingsTabs")) {
            drawGeneral(*project);
            drawRendering(*project);
            drawEditor(*project);
            drawAudio(*project);
            drawAutoloads(*project);
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void SettingsWindow::drawAppearance() {
    ImGui::SeparatorText("Appearance");
    int themeIndex = lightTheme_ ? 1 : 0;
    const char* themeNames[] = {"Saida Dark", "Saida Light"};
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::Combo("Editor Theme", &themeIndex, themeNames, 2)) {
        lightTheme_ = themeIndex == 1;
        applyStyle();
        saveLightThemePreference(lightTheme_);
    }
    ImGui::TextDisabled(
        "Saida Dark is the default. The 3D viewport stays dark in both themes.");
    ImGui::Spacing();
}

void SettingsWindow::drawGeneral(Project& project) {
    if (!ImGui::BeginTabItem("General")) return;
    ImGui::Spacing();
    // Renaming is atomic only while the project is closed, so it stays in Hub.
    char name[128];
    std::snprintf(name, sizeof(name), "%s", project.name().c_str());
    ImGui::BeginDisabled();
    ImGui::InputText("Project Name", name, sizeof(name),
                     ImGuiInputTextFlags_ReadOnly);
    ImGui::EndDisabled();
    ImGui::TextDisabled("Rename from the Hub (project must be closed).");

    ImGui::SeparatorText("Performance");
    int maxFps = project.maxFps();
    if (ImGui::InputInt("Max FPS (0 = Unlimited)", &maxFps)) {
        project.setMaxFps(std::max(0, maxFps));
        project.save();
    }
    ImGui::SeparatorText("Metadata");
    ImGui::TextDisabled("Version: 1.0.0");
    ImGui::TextDisabled("Company Name: DefaultCompany");
    ImGui::EndTabItem();
}

void SettingsWindow::drawRendering(Project& project) {
    if (!ImGui::BeginTabItem("Rendering")) return;
    ImGui::Spacing();
    ImGui::SeparatorText("Mesh LODs");
    bool autoLods = project.autoMeshLods();
    if (ImGui::Checkbox("Auto Mesh LODs##Rendering", &autoLods)) {
        project.setAutoMeshLods(autoLods);
        project.save();
    }
    ImGui::TextDisabled(
        "Imports .glb through AutoLOD and adds editable LOD Group components.");

    ImGui::Spacing();
    ImGui::SeparatorText("Shadows");
    constexpr int resolutions[] = {512, 1024, 2048, 4096, 8192};
    int resolutionIndex = 0;
    for (int i = 0; i < 5; ++i)
        if (project.shadowResolution() == resolutions[i]) resolutionIndex = i;
    const char* names[] = {
        "Very Low (512)", "Low (1024)", "Medium (2048)",
        "High (4096)", "Ultra (8192)",
    };
    if (ImGui::Combo("Resolution", &resolutionIndex, names, 5))
        project.setShadowResolution(resolutions[resolutionIndex]);

    float distance = project.shadowDistance();
    if (ImGui::DragFloat("Distance", &distance, 0.5f, 10.0f, 200.0f))
        project.setShadowDistance(distance);
    float softness = project.shadowSoftness();
    if (ImGui::DragFloat("Softness (Blur)", &softness, 0.05f, 0.0f, 10.0f))
        project.setShadowSoftness(softness);

    ImGui::SeparatorText("Global Quality");
    const char* msaaNames[] = {"Off", "2x MSAA", "4x MSAA", "8x MSAA"};
    ImGui::Combo("Anti-aliasing", &msaa_, msaaNames, 4);
    bool vsync = project.vSync();
    if (ImGui::Checkbox("V-Sync", &vsync)) {
        project.setVSync(vsync);
        project.save();
    }
    ImGui::Spacing();
    ImGui::TextDisabled(
        "(Note: Anti-aliasing will be wired to the Vulkan backend later)");
    ImGui::EndTabItem();
}

void SettingsWindow::drawEditor(Project& project) {
    if (!ImGui::BeginTabItem("Editor")) return;
    ImGui::Spacing();
    ImGui::SeparatorText("Preferences");
    bool showColliders = project.showColliders();
    if (ImGui::Checkbox("Show Colliders", &showColliders)) {
        project.setShowColliders(showColliders);
        project.save();
    }
    ImGui::Checkbox("Auto-save Project", &autoSave_);
    ImGui::SliderFloat("Gizmo Size", &gizmoSize_, 0.1f, 3.0f);
    ImGui::EndTabItem();
}

void SettingsWindow::drawAudio(Project& project) {
    if (!ImGui::BeginTabItem("Audio")) return;
    ImGui::Spacing();
    ImGui::SeparatorText("Global");
    float masterVolume = project.masterVolume();
    if (ImGui::SliderFloat("Master Volume", &masterVolume, 0.0f, 1.0f)) {
        project.setMasterVolume(masterVolume);
        AudioManager::get().setMasterVolume(masterVolume);
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Default Settings");
    AudioSettings defaults = project.defaultAudioSettings();
    bool changed = false;
    changed |= ImGui::SliderFloat(
        "Default Volume", &defaults.volume, 0.0f, 1.0f);
    changed |= ImGui::Checkbox("Loop by Default", &defaults.loop);
    changed |= ImGui::Checkbox(
        "Spatialized by Default", &defaults.spatialized);
    if (defaults.spatialized) {
        changed |= ImGui::DragFloat(
            "Min Distance", &defaults.minDistance, 0.5f, 0.1f, 1000.0f);
        changed |= ImGui::DragFloat(
            "Max Distance", &defaults.maxDistance, 1.0f, 1.0f, 10000.0f);
    }
    if (changed) {
        project.defaultAudioSettings() = defaults;
        AudioManager::get().setDefaultSettings(defaults);
    }
    drawAudioAliases(project);
    ImGui::EndTabItem();
}

void SettingsWindow::drawAudioAliases(Project& project) {
    ImGui::Spacing();
    ImGui::SeparatorText("Audio Assets Aliases");
    ImGui::TextDisabled("Map friendly names to audio files.");
    std::string aliasToRemove;
    if (ImGui::BeginTable(
            "AudioAliasesTable", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(
            "Alias Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(
            "File Path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(
            "##Action", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableHeadersRow();
        for (const auto& [name, path] : project.audioAliases()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", path.c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(name.c_str());
            if (ImGui::Button("X", ImVec2(24, 0))) aliasToRemove = name;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!aliasToRemove.empty()) project.removeAudioAlias(aliasToRemove);

    ImGui::Spacing();
    ImGui::SeparatorText("Register New Audio Alias");
    ImGui::TextDisabled(
        "Drag & Drop .ogg files below, or type manually.");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint(
        "##AliasName", "Alias Name (e.g. 'Explosion')",
        newAliasName_, sizeof(newAliasName_));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint(
        "##AliasPath", "File Path (e.g. 'assets/audio/exp.ogg')",
        newAliasPath_, sizeof(newAliasPath_));

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload("FILE_AUDIO")) {
            const std::filesystem::path file(
                static_cast<const char*>(payload->Data));
            if (file.extension() == ".ogg") {
                std::string relative =
                    std::filesystem::relative(
                        file, project.rootPath()).string();
                std::replace(relative.begin(), relative.end(), '\\', '/');
                std::snprintf(
                    newAliasPath_, sizeof(newAliasPath_), "%s",
                    relative.c_str());
                if (newAliasName_[0] == '\0')
                    std::snprintf(
                        newAliasName_, sizeof(newAliasName_), "%s",
                        file.stem().string().c_str());
                project.assetRegistry().registerAsset(
                    relative, AssetType::Audio);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::Button("Add / Update Alias", ImVec2(-1, 0)) &&
        newAliasName_[0] != '\0' && newAliasPath_[0] != '\0') {
        project.setAudioAlias(newAliasName_, newAliasPath_);
        newAliasName_[0] = '\0';
        newAliasPath_[0] = '\0';
    }
}

void SettingsWindow::drawAutoloads(Project& project) {
    if (!ImGui::BeginTabItem("Autoloads")) return;
    ImGui::TextDisabled(
        "Persistent singletons spawned into the World at play time.");
    ImGui::TextDisabled(
        "They survive scene changes (game state, save, persistent UI...).");
    ImGui::TextDisabled(
        "Value: a 'scenes/X.scene' prefab path, or a behaviour type name.");
    ImGui::Spacing();

    std::string toRemove;
    if (ImGui::BeginTable(
            "AutoloadsTable", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(
            "Scene / Type", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(
            "##Action", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableHeadersRow();
        for (const auto& [name, value] : project.autoloads()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", value.c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(name.c_str());
            if (ImGui::Button("X", ImVec2(24, 0))) toRemove = name;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!toRemove.empty()) project.removeAutoload(toRemove);

    ImGui::Spacing();
    ImGui::SeparatorText("Register New Autoload");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint(
        "##AutoName", "Name (e.g. 'GameState')",
        newAutoloadName_, sizeof(newAutoloadName_));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint(
        "##AutoVal", "scenes/X.scene  or  BehaviourType",
        newAutoloadValue_, sizeof(newAutoloadValue_));

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload("FILE_SCENE")) {
            const std::filesystem::path file(
                static_cast<const char*>(payload->Data));
            std::string relative =
                std::filesystem::relative(
                    file, project.rootPath()).string();
            std::replace(relative.begin(), relative.end(), '\\', '/');
            std::snprintf(
                newAutoloadValue_, sizeof(newAutoloadValue_), "%s",
                relative.c_str());
            if (newAutoloadName_[0] == '\0')
                std::snprintf(
                    newAutoloadName_, sizeof(newAutoloadName_), "%s",
                    file.stem().string().c_str());
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::Button("Add / Update Autoload", ImVec2(-1, 0)) &&
        newAutoloadName_[0] != '\0' &&
        newAutoloadValue_[0] != '\0') {
        project.setAutoload(newAutoloadName_, newAutoloadValue_);
        newAutoloadName_[0] = '\0';
        newAutoloadValue_[0] = '\0';
    }
    ImGui::EndTabItem();
}


} // namespace saida
