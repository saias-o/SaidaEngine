#pragma once

namespace saida {

class Project;

// Density of the editor chrome. Big is the roomy Saida rhythm; Small drops to
// Dear ImGui's own font size and spacing so the editor fits a small screen.
enum class EditorUiDensity { Big, Small };

// Owns persistent editor preferences and renders each settings tab through a
// bounded section. Theme and density are applied and persisted atomically on
// change.
class SettingsWindow {
public:
    SettingsWindow();

    void requestOpen() { open_ = true; }
    void draw(Project* project);
    bool lightTheme() const { return lightTheme_; }
    EditorUiDensity uiDensity() const { return density_; }

private:
    void applyStyle() const;
    void applyLayout() const;
    void applyColors() const;
    void drawAppearance();
    void drawGeneral(Project& project);
    void drawRendering(Project& project);
    void drawEditor(Project& project);
    void drawAudio(Project& project);
    void drawAudioAliases(Project& project);
    void drawAutoloads(Project& project);

    bool open_ = false;
    bool lightTheme_ = false;
    EditorUiDensity density_ = EditorUiDensity::Big;
    int msaa_ = 0;
    bool autoSave_ = true;
    float gizmoSize_ = 1.0f;
    char newAliasName_[64] = "";
    char newAliasPath_[256] = "";
    char newAutoloadName_[64] = "";
    char newAutoloadValue_[256] = "";
};

} // namespace saida
