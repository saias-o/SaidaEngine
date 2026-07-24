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

private:
    UIInteractableNode* raycast(UINode* node, const glm::vec2& mousePos);

    UIInteractableNode* hoveredNode_ = nullptr;
    UIInteractableNode* pressedNode_ = nullptr;
    WebCanvasNode* hoveredWebCanvas_ = nullptr;
    WebCanvasNode* focusedWebCanvas_ = nullptr;
    std::unordered_map<uint64_t, WebCanvasNode*> touchTargets_;
    std::unordered_map<uint64_t, glm::vec2> touchLocalPositions_;
};

} // namespace saida
