#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <unordered_map>

namespace saida {

class Camera;
class Scene;
class UINode;
class UIInteractableNode;
class WebCanvasNode;

class UIInteractionSystem {
public:
    UIInteractionSystem() = default;
    ~UIInteractionSystem() = default;

    // Returns true if a UI node handled the event (hovered or pressed)
    bool update(Scene& scene, const Camera& camera, const glm::vec2& mousePos, const glm::vec2& viewportSize,
                bool isLeftDown, bool isLeftJustPressed, bool isLeftJustReleased);

    // Keyboard focus given by the application rather than by a click: a text
    // field opened from a key while the cursor is captured has no click to
    // focus its canvas. The canvas then receives keys and text and captures
    // the keyboard (Input::uiCapturesKeyboard) from the next update, until a
    // click elsewhere, its removal from the scene, or focusWebCanvas(nullptr).
    // The next update revalidates it against the scene, as every stored target.
    void focusWebCanvas(WebCanvasNode* canvas) {
        focusedWebCanvas_ = canvas;
        hierarchyVersion_ = ~uint64_t(0);
    }
    WebCanvasNode* focusedWebCanvas() const { return focusedWebCanvas_; }

private:
    UIInteractableNode* raycast(UINode* node, const glm::vec2& mousePos);

    UIInteractableNode* hoveredNode_ = nullptr;
    UIInteractableNode* pressedNode_ = nullptr;
    WebCanvasNode* hoveredWebCanvas_ = nullptr;
    WebCanvasNode* focusedWebCanvas_ = nullptr;
    std::unordered_map<uint64_t, WebCanvasNode*> touchTargets_;
    std::unordered_map<uint64_t, glm::vec2> touchLocalPositions_;
    uint64_t hierarchyVersion_ = 0;
};

} // namespace saida
