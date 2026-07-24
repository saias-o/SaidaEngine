#pragma once

namespace saida {

class Project;

// Owns persistent editor preferences and renders each settings tab through a
// bounded section. Theme state is applied and persisted atomically on change.
class SettingsWindow {
public:
    SettingsWindow();

    void requestOpen() { open_ = true; }
    void draw(Project* project);
    bool lightTheme() const { return lightTheme_; }

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
    int msaa_ = 0;
    bool autoSave_ = true;
    float gizmoSize_ = 1.0f;
    char newAliasName_[64] = "";
    char newAliasPath_[256] = "";
    char newAutoloadName_[64] = "";
    char newAutoloadValue_[256] = "";
};

} // namespace saida
