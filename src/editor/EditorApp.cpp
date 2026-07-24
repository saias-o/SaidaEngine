#include "editor/EditorApp.hpp"
#include "Engine.hpp"

#include "core/Camera.hpp"
#include "core/Input.hpp"
#include "core/Log.hpp"
#include "core/Time.hpp"
#include "core/Window.hpp"
#include "scene/Scene.hpp"

#include "imgui.h"

#include "audio/AudioManager.hpp"
#include "scene/SceneTree.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace saida {

EditorApp::EditorApp(Engine& engine) : engine_(engine) {
    Time::setScale(0.0f); // Default to editor (paused) mode
}

int EditorApp::runAutomatedBuild(bool web, const std::string& outputDir) {
    std::string message;
    const bool ok = ui_.runAutomatedBuild(&engine_.project(), web, outputDir, &message);
    if (ok)
        Log::info("[BUILD] PASS ", message);
    else
        Log::error("[BUILD] FAIL: ", message);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

void EditorApp::setPlayMode(bool play) {
    if (playMode_ == play) return;
    // Defer: the call site is typically inside ui_.draw() (the Scene/Play button)
    // or processInput(), both of which run while the panels still hold the live
    // Scene pointer. Mounting/unmounting the world here would free that Scene
    // mid-frame. applyPlayMode() runs at the end of update(), when it's safe.
    pendingPlayMode_ = play ? 1 : 0;
}

void EditorApp::applyPlayMode(bool play) {
    if (playMode_ == play) return;

    ui_.clearSelection();  // selection refers to a tree that's about to swap

    if (play) {
        // Some presentation modes require an isolated runtime process. The Engine
        // owns that policy; the editor only issues a generic Play request.
#ifdef SAIDA_ENABLE_XR
        if (engine_.launchExternalPreviewIfNeeded()) {
            return;
        }
#endif
        // Mount the persistent World from a snapshot of the edit scene (which is
        // left untouched — so Stop needs no restore). Autoloads spawn here.
        engine_.mountWorld();
        playMode_ = true;
        Time::setScale(1.0f);
    } else {
        playMode_ = false;
        Time::setScale(0.0f);
        engine_.unmountWorld();  // edit scene is pristine; just drop the World
        AudioManager::get().stopAllGlobal();
    }
}

void EditorApp::update(float dt) {
    processInput(dt);

    // In Play, edit/inspect the live World sub-scene; otherwise the edit document.
    Scene* shown = (isPlayMode() && engine_.sceneTree().mounted())
                       ? &engine_.sceneTree().currentScene()
                       : &engine_.scene();
    ui_.draw(this, shown, &engine_.camera(), &engine_.project(), &engine_.resources(), dt);
    engine_.setRenderViewport(ui_.viewportPosition(), ui_.viewportSize());

    // The render target during Play is the World (set by mountWorld); leave it.
    if (!isPlayMode()) {
        if (ui_.isPreviewMode())
            engine_.setSceneOverride(ui_.previewScene());
        else
            engine_.setSceneOverride(nullptr);
    }

    // Now that the UI frame is fully recorded and no panel holds the live Scene
    // pointer anymore, it's safe to mount/unmount the world for any Play/Stop
    // requested during this frame.
    if (pendingPlayMode_ != -1) {
        bool play = pendingPlayMode_ == 1;
        pendingPlayMode_ = -1;
        applyPlayMode(play);
    }
}

void EditorApp::updateCursorCapture(bool isPlayMode) {
    Window& window = engine_.window();

    if (!isPlayMode && wasPlayMode_) {
        window.setCursorCaptured(false);  // release on leaving play mode
    }
    wasPlayMode_ = isPlayMode;

    if (isPlayMode) {
        if (Input::isKeyPressed(KeyCode::Tab))
            window.setCursorCaptured(!window.cursorCaptured());

        // Auto-capture if clicking inside the viewport during play mode
        if (Input::isMouseButtonPressed(MouseButton::Left) && !window.cursorCaptured()) {
            if (ui_.isViewportHovered(Input::mousePosition().x, Input::mousePosition().y)) {
                window.setCursorCaptured(true);
            }
        }
    } else {
        // Scene mode: fly only while holding right-click over the viewport.
        bool rightClick = Input::isMouseButtonDown(MouseButton::Right);
        if (rightClick && !window.cursorCaptured()) {
            if (!ImGui::GetIO().WantCaptureMouse)
                window.setCursorCaptured(true);
        } else if (!rightClick && window.cursorCaptured()) {
            window.setCursorCaptured(false);
        }
    }
}

void EditorApp::processInput(float dt) {
    Window& window = engine_.window();
    Camera& camera = engine_.camera();

    if (Input::isKeyPressed(KeyCode::Escape)) {
        if (isPlayMode()) {
            setPlayMode(false);  // exit play mode back to scene mode
            window.setCursorCaptured(false);
        } else if (window.cursorCaptured()) {
            window.setCursorCaptured(false);
        } else {
            window.close();
            return;
        }
    }

    if (ui_.quitRequested()) {
        window.close();
        return;
    }

    updateCursorCapture(isPlayMode());

    // Cursor free (UI mode): ImGui owns the mouse. But over the 3D viewport,
    // allow pan (middle-drag) and zoom (wheel).
    if (!window.cursorCaptured()) {
        ImGuiIO& io = ImGui::GetIO();
        bool inViewport = ui_.isViewportHovered(Input::mousePosition().x, Input::mousePosition().y);
        if (inViewport || !io.WantCaptureMouse) {
            if (Input::isMouseButtonDown(MouseButton::Middle)) {
                glm::vec2 d = Input::mouseDelta();
                constexpr float panSpeed = 0.01f;
                camera.position -= camera.right() * d.x * panSpeed;
                camera.position += camera.up() * d.y * panSpeed;
            }
            camera.position += camera.front() * io.MouseWheel * 2.0f;
        }
        return; // Bail out! Don't process FPS fly controls.
    }

    // Right-click fly mode - only in Scene Mode! (cf. Unity/Unreal/Godot navigation)
    if (isPlayMode()) {
        return; // The editor camera should not be controlled by WASD/mouse in Play Mode.
    }

    // Mouse look: pixel delta → degrees (sensitivity tuned to feel like 3D DCC tools).
    constexpr float sensitivity = 0.1f;  // degrees per pixel
    glm::vec2 d = Input::mouseDelta();
    camera.rotate(d.x * sensitivity, -d.y * sensitivity);  // screen Y is down

    // Scroll wheel adjusts the fly speed while navigating (like Unity/Unreal/Godot).
    float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f)
        flySpeed_ = std::clamp(flySpeed_ * std::pow(1.15f, wheel), 0.5f, 200.0f);

    glm::vec3 front = camera.front();
    glm::vec3 right = camera.right();
    float speed = flySpeed_ * (Input::isKeyDown(KeyCode::LeftShift) ? 3.0f : 1.0f) * dt;

    if (Input::isKeyDown(KeyCode::W)) camera.position += front * speed;
    if (Input::isKeyDown(KeyCode::S)) camera.position -= front * speed;
    if (Input::isKeyDown(KeyCode::D)) camera.position += right * speed;
    if (Input::isKeyDown(KeyCode::A)) camera.position -= right * speed;
    if (Input::isKeyDown(KeyCode::Space)) camera.position += glm::vec3(0, 1, 0) * speed;
    if (Input::isKeyDown(KeyCode::LeftControl)) camera.position -= glm::vec3(0, 1, 0) * speed;
}

} // namespace saida
