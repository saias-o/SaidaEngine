#pragma once

#include "editor/EditorUI.hpp"

namespace saida {

class Engine;

// The editor application layer on top of an Engine. Owns the editor UI and the
// editor/viewport input (free-fly camera, pan/zoom, play-mode cursor). Driven
// once per frame via Engine::setOnFrame -> EditorApp::update. Lives in the
// `saida_editor` target, so the engine library stays editor-agnostic.
class EditorApp {
public:
    explicit EditorApp(Engine& engine);

    void update(float dt);  // input + UI; call from the engine's onFrame hook

    // Automated Build click (--build flag): runs the same code as the Build
    // dialog's button on the loaded project, logs [BUILD] PASS/FAIL, and
    // returns the process's exit code. No UI frame required.
    int runAutomatedBuild(bool web, const std::string& outputDir);

    bool isPlayMode() const { return playMode_; }
    // Requests a Play/Stop transition. The actual world mount/unmount is DEFERRED
    // to the end of update(): tearing down the world mid-UI-frame would free the
    // Scene that the panels (hierarchy/inspector/gizmo) are still drawing this
    // frame — a use-after-free. See applyPlayMode().
    void setPlayMode(bool play);

private:
    void processInput(float dt);
    void updateCursorCapture(bool isPlayMode);
    void applyPlayMode(bool play);  // does the real mount/unmount (deferred)

    Engine& engine_;
    EditorUI ui_;
    bool playMode_ = false;
    bool wasPlayMode_ = false;
    int pendingPlayMode_ = -1;  // -1 none, 0 stop, 1 play (applied at end of update)
    float flySpeed_ = 6.0f;     // editor fly speed (units/s); scroll wheel adjusts it
};

} // namespace saida
