#include "ui/UIInteractionSystem.hpp"

#include "core/Camera.hpp"
#include "core/Input.hpp"
#include "scene/Scene.hpp"
#include "nodes/UICanvasNode.hpp"
#include "nodes/UIInteractableNode.hpp"
#include "nodes/WebCanvasNode.hpp"

#include <RmlUi/Core/Input.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace saida {

namespace {

struct KeyMap {
    KeyCode key;
    Rml::Input::KeyIdentifier rml;
};

constexpr KeyMap kKeyMap[] = {
    {KeyCode::Space, Rml::Input::KI_SPACE},
    {KeyCode::Num0, Rml::Input::KI_0}, {KeyCode::Num1, Rml::Input::KI_1},
    {KeyCode::Num2, Rml::Input::KI_2}, {KeyCode::Num3, Rml::Input::KI_3},
    {KeyCode::Num4, Rml::Input::KI_4}, {KeyCode::Num5, Rml::Input::KI_5},
    {KeyCode::Num6, Rml::Input::KI_6}, {KeyCode::Num7, Rml::Input::KI_7},
    {KeyCode::Num8, Rml::Input::KI_8}, {KeyCode::Num9, Rml::Input::KI_9},
    {KeyCode::A, Rml::Input::KI_A}, {KeyCode::B, Rml::Input::KI_B},
    {KeyCode::C, Rml::Input::KI_C}, {KeyCode::D, Rml::Input::KI_D},
    {KeyCode::E, Rml::Input::KI_E}, {KeyCode::F, Rml::Input::KI_F},
    {KeyCode::G, Rml::Input::KI_G}, {KeyCode::H, Rml::Input::KI_H},
    {KeyCode::I, Rml::Input::KI_I}, {KeyCode::J, Rml::Input::KI_J},
    {KeyCode::K, Rml::Input::KI_K}, {KeyCode::L, Rml::Input::KI_L},
    {KeyCode::M, Rml::Input::KI_M}, {KeyCode::N, Rml::Input::KI_N},
    {KeyCode::O, Rml::Input::KI_O}, {KeyCode::P, Rml::Input::KI_P},
    {KeyCode::Q, Rml::Input::KI_Q}, {KeyCode::R, Rml::Input::KI_R},
    {KeyCode::S, Rml::Input::KI_S}, {KeyCode::T, Rml::Input::KI_T},
    {KeyCode::U, Rml::Input::KI_U}, {KeyCode::V, Rml::Input::KI_V},
    {KeyCode::W, Rml::Input::KI_W}, {KeyCode::X, Rml::Input::KI_X},
    {KeyCode::Y, Rml::Input::KI_Y}, {KeyCode::Z, Rml::Input::KI_Z},
    {KeyCode::Semicolon, Rml::Input::KI_OEM_1}, {KeyCode::Equal, Rml::Input::KI_OEM_PLUS},
    {KeyCode::Comma, Rml::Input::KI_OEM_COMMA}, {KeyCode::Minus, Rml::Input::KI_OEM_MINUS},
    {KeyCode::Period, Rml::Input::KI_OEM_PERIOD}, {KeyCode::Slash, Rml::Input::KI_OEM_2},
    {KeyCode::GraveAccent, Rml::Input::KI_OEM_3}, {KeyCode::LeftBracket, Rml::Input::KI_OEM_4},
    {KeyCode::Backslash, Rml::Input::KI_OEM_5}, {KeyCode::RightBracket, Rml::Input::KI_OEM_6},
    {KeyCode::Apostrophe, Rml::Input::KI_OEM_7},
    {KeyCode::Keypad0, Rml::Input::KI_NUMPAD0}, {KeyCode::Keypad1, Rml::Input::KI_NUMPAD1},
    {KeyCode::Keypad2, Rml::Input::KI_NUMPAD2}, {KeyCode::Keypad3, Rml::Input::KI_NUMPAD3},
    {KeyCode::Keypad4, Rml::Input::KI_NUMPAD4}, {KeyCode::Keypad5, Rml::Input::KI_NUMPAD5},
    {KeyCode::Keypad6, Rml::Input::KI_NUMPAD6}, {KeyCode::Keypad7, Rml::Input::KI_NUMPAD7},
    {KeyCode::Keypad8, Rml::Input::KI_NUMPAD8}, {KeyCode::Keypad9, Rml::Input::KI_NUMPAD9},
    {KeyCode::KeypadEnter, Rml::Input::KI_NUMPADENTER},
    {KeyCode::KeypadMultiply, Rml::Input::KI_MULTIPLY}, {KeyCode::KeypadAdd, Rml::Input::KI_ADD},
    {KeyCode::KeypadSubtract, Rml::Input::KI_SUBTRACT}, {KeyCode::KeypadDecimal, Rml::Input::KI_DECIMAL},
    {KeyCode::KeypadDivide, Rml::Input::KI_DIVIDE}, {KeyCode::Backspace, Rml::Input::KI_BACK},
    {KeyCode::Tab, Rml::Input::KI_TAB}, {KeyCode::Enter, Rml::Input::KI_RETURN},
    {KeyCode::Pause, Rml::Input::KI_PAUSE}, {KeyCode::CapsLock, Rml::Input::KI_CAPITAL},
    {KeyCode::Escape, Rml::Input::KI_ESCAPE}, {KeyCode::PageUp, Rml::Input::KI_PRIOR},
    {KeyCode::PageDown, Rml::Input::KI_NEXT}, {KeyCode::End, Rml::Input::KI_END},
    {KeyCode::Home, Rml::Input::KI_HOME}, {KeyCode::Left, Rml::Input::KI_LEFT},
    {KeyCode::Up, Rml::Input::KI_UP}, {KeyCode::Right, Rml::Input::KI_RIGHT},
    {KeyCode::Down, Rml::Input::KI_DOWN}, {KeyCode::PrintScreen, Rml::Input::KI_SNAPSHOT},
    {KeyCode::Insert, Rml::Input::KI_INSERT}, {KeyCode::Delete, Rml::Input::KI_DELETE},
    {KeyCode::LeftShift, Rml::Input::KI_LSHIFT}, {KeyCode::RightShift, Rml::Input::KI_RSHIFT},
    {KeyCode::LeftControl, Rml::Input::KI_LCONTROL}, {KeyCode::RightControl, Rml::Input::KI_RCONTROL},
    {KeyCode::LeftAlt, Rml::Input::KI_LMENU}, {KeyCode::RightAlt, Rml::Input::KI_RMENU},
    {KeyCode::LeftSuper, Rml::Input::KI_LWIN}, {KeyCode::RightSuper, Rml::Input::KI_RWIN},
    {KeyCode::Menu, Rml::Input::KI_APPS},
};

int rmlModifiers() {
    int modifiers = 0;
    if (Input::isKeyDown(KeyCode::LeftControl) || Input::isKeyDown(KeyCode::RightControl)) modifiers |= Rml::Input::KM_CTRL;
    if (Input::isKeyDown(KeyCode::LeftShift) || Input::isKeyDown(KeyCode::RightShift)) modifiers |= Rml::Input::KM_SHIFT;
    if (Input::isKeyDown(KeyCode::LeftAlt) || Input::isKeyDown(KeyCode::RightAlt)) modifiers |= Rml::Input::KM_ALT;
    if (Input::isKeyDown(KeyCode::LeftSuper) || Input::isKeyDown(KeyCode::RightSuper)) modifiers |= Rml::Input::KM_META;
    if (Input::isKeyDown(KeyCode::CapsLock)) modifiers |= Rml::Input::KM_CAPSLOCK;
    if (Input::isKeyDown(KeyCode::NumLock)) modifiers |= Rml::Input::KM_NUMLOCK;
    if (Input::isKeyDown(KeyCode::ScrollLock)) modifiers |= Rml::Input::KM_SCROLLLOCK;
    return modifiers;
}

WebCanvasNode::MouseButton webButton(MouseButton button) {
    switch (button) {
    case MouseButton::Right: return WebCanvasNode::MouseButton::Right;
    case MouseButton::Middle: return WebCanvasNode::MouseButton::Middle;
    default: return WebCanvasNode::MouseButton::Left;
    }
}

WebCanvasNode::TouchEvent webTouchEvent(TouchPhase phase) {
    switch (phase) {
    case TouchPhase::Began: return WebCanvasNode::TouchEvent::Start;
    case TouchPhase::Ended: return WebCanvasNode::TouchEvent::End;
    case TouchPhase::Cancelled: return WebCanvasNode::TouchEvent::Cancel;
    case TouchPhase::Moved:
    default: return WebCanvasNode::TouchEvent::Move;
    }
}

glm::vec3 screenRayDirection(const Camera& camera, glm::vec2 point, glm::vec2 viewportSize) {
    if (viewportSize.x <= 0.0f || viewportSize.y <= 0.0f) return camera.front();
    float ndcX = (point.x / viewportSize.x) * 2.0f - 1.0f;
    float ndcY = 1.0f - (point.y / viewportSize.y) * 2.0f;
    float aspect = viewportSize.x / viewportSize.y;
    float tanHalfFov = std::tan(camera.fovDegrees * 0.017453292519943295f * 0.5f);
    return glm::normalize(camera.front()
        + camera.right() * ndcX * aspect * tanHalfFov
        + camera.up() * ndcY * tanHalfFov);
}

struct WebHit {
    WebCanvasNode* canvas = nullptr;
    glm::vec2 local{0.0f};
    float distance = std::numeric_limits<float>::max();
};

std::vector<WebCanvasNode*> activeWebCanvases(Scene& scene) {
    std::vector<WebCanvasNode*> canvases;
    for (WebCanvasNode* canvas : scene.webCanvases()) {
        if (canvas && canvas->isActiveInHierarchy() && canvas->interactive()) {
            canvases.push_back(canvas);
        }
    }
    std::stable_sort(canvases.begin(), canvases.end(), [](const WebCanvasNode* a, const WebCanvasNode* b) {
        return a->renderOrder() < b->renderOrder();
    });
    return canvases;
}

WebHit findWebCanvas(Scene& scene, const Camera& camera, glm::vec2 point, glm::vec2 viewportSize) {
    WebHit hit;
    std::vector<WebCanvasNode*> canvases = activeWebCanvases(scene);
    for (auto it = canvases.rbegin(); it != canvases.rend(); ++it) {
        WebCanvasNode* wcn = *it;
        if (wcn->mode() == WebCanvasNode::Mode::ScreenSpace && wcn->screenContains(point)) {
            glm::vec2 local = wcn->screenToLocal(point);
            if (!wcn->hitTest(local)) continue;
            hit.canvas = wcn;
            hit.local = local;
            hit.distance = -1.0f;
            return hit;
        }
    }

    glm::vec3 origin = camera.position;
    glm::vec3 direction = screenRayDirection(camera, point, viewportSize);
    for (WebCanvasNode* wcn : canvases) {
        if (wcn->mode() != WebCanvasNode::Mode::WorldSpace) continue;
        glm::vec2 local{0.0f};
        float distance = 0.0f;
        if (wcn->raycast(origin, direction, local, distance) &&
            wcn->hitTest(local) && distance < hit.distance) {
            hit.canvas = wcn;
            hit.local = local;
            hit.distance = distance;
        }
    }
    return hit;
}

glm::vec2 clampCanvasLocal(WebCanvasNode& canvas, glm::vec2 local) {
    return {
        std::clamp(local.x, 0.0f, static_cast<float>(canvas.width())),
        std::clamp(local.y, 0.0f, static_cast<float>(canvas.height()))
    };
}

} // namespace

bool UIInteractionSystem::update(Scene& scene, const Camera& camera, const glm::vec2& mousePos, const glm::vec2& viewportSize,
                                 bool isLeftDown, bool isLeftJustPressed, bool isLeftJustReleased) {
    UICanvasNode* canvas = scene.uiCanvas();
    if (!canvas || !canvas->isActiveInHierarchy()) {
        if (hoveredNode_) {
            hoveredNode_->onHoverExit();
            hoveredNode_ = nullptr;
        }
    } else {
        for (auto& child : canvas->children()) {
            if (auto* uiChild = dynamic_cast<UINode*>(child.get())) {
                uiChild->updateTransforms(0.0f, 0.0f, viewportSize.x, viewportSize.y);
            }
        }
    }

    UIInteractableNode* targetNode = nullptr;
    if (canvas && canvas->isActiveInHierarchy()) {
        const auto& children = canvas->children();
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            if (UINode* uiChild = dynamic_cast<UINode*>(it->get())) {
                targetNode = raycast(uiChild, mousePos);
                if (targetNode) break;
            }
        }
    }

    if (targetNode != hoveredNode_) {
        if (hoveredNode_) hoveredNode_->onHoverExit();
        hoveredNode_ = targetNode;
        if (hoveredNode_) hoveredNode_->onHoverEnter();
    }

    if (isLeftJustPressed) {
        if (hoveredNode_) {
            hoveredNode_->onMouseDown();
            pressedNode_ = hoveredNode_;
        } else {
            pressedNode_ = nullptr;
        }
    }

    if (isLeftJustReleased) {
        if (pressedNode_) {
            if (pressedNode_ == hoveredNode_) pressedNode_->onClick();
            pressedNode_->onMouseUp();
            pressedNode_ = nullptr;
        }
    }

    if (!isLeftDown && pressedNode_) {
        pressedNode_->onMouseUp();
        pressedNode_ = nullptr;
    }

    WebHit mouseHit = findWebCanvas(scene, camera, mousePos, viewportSize);
    WebCanvasNode* webTarget = mouseHit.canvas;

    bool webHandled = false;
    const int modifiers = rmlModifiers();
    if (webTarget) {
        hoveredWebCanvas_ = webTarget;
        glm::vec2 local = clampCanvasLocal(*webTarget, mouseHit.local);
        int mx = static_cast<int>(local.x);
        int my = static_cast<int>(local.y);
        webHandled = webTarget->fireMouseEvent(WebCanvasNode::MouseEvent::Move, mx, my,
            isLeftDown ? WebCanvasNode::MouseButton::Left : WebCanvasNode::MouseButton::None, modifiers) || webHandled;

        const std::array<MouseButton, 3> buttons{MouseButton::Left, MouseButton::Right, MouseButton::Middle};
        for (MouseButton button : buttons) {
            if (Input::isMouseButtonPressed(button)) {
                focusedWebCanvas_ = webTarget;
                webHandled = webTarget->fireMouseEvent(WebCanvasNode::MouseEvent::Down, mx, my, webButton(button), modifiers) || true;
            }
            if (Input::isMouseButtonReleased(button)) {
                webHandled = webTarget->fireMouseEvent(WebCanvasNode::MouseEvent::Up, mx, my, webButton(button), modifiers) || webHandled;
            }
        }

        glm::vec2 scroll = Input::scrollDelta();
        if (scroll.x != 0.0f || scroll.y != 0.0f) {
            webHandled = webTarget->fireScrollEvent(scroll.x, scroll.y, modifiers) || true;
        }
    } else {
        hoveredWebCanvas_ = nullptr;
        if (isLeftJustPressed) focusedWebCanvas_ = nullptr;
    }

    for (const TouchPoint& touch : Input::touches()) {
        WebCanvasNode* touchTarget = nullptr;
        glm::vec2 local{0.0f};
        auto active = touchTargets_.find(touch.id);
        if (active != touchTargets_.end() && active->second && active->second->isActiveInHierarchy()) {
            WebHit hit = findWebCanvas(scene, camera, touch.position, viewportSize);
            if (hit.canvas == active->second) {
                touchTarget = active->second;
                local = hit.local;
            } else if (active->second->mode() == WebCanvasNode::Mode::ScreenSpace) {
                touchTarget = active->second;
                local = active->second->screenToLocal(touch.position);
            } else if (auto last = touchLocalPositions_.find(touch.id); last != touchLocalPositions_.end()) {
                touchTarget = active->second;
                local = last->second;
            }
        }
        if (!touchTarget && touch.phase != TouchPhase::Ended && touch.phase != TouchPhase::Cancelled) {
            WebHit hit = findWebCanvas(scene, camera, touch.position, viewportSize);
            touchTarget = hit.canvas;
            local = hit.local;
            if (touchTarget && touch.phase == TouchPhase::Began) {
                touchTargets_[touch.id] = touchTarget;
            }
        }
        if (!touchTarget) continue;

        local = clampCanvasLocal(*touchTarget, local);
        focusedWebCanvas_ = touchTarget;
        hoveredWebCanvas_ = touchTarget;
        webHandled = touchTarget->fireTouchEvent(touch.id, local, webTouchEvent(touch.phase), modifiers) || true;
        if (touch.phase == TouchPhase::Ended || touch.phase == TouchPhase::Cancelled) {
            touchTargets_.erase(touch.id);
            touchLocalPositions_.erase(touch.id);
        } else {
            touchLocalPositions_[touch.id] = local;
        }
    }

    if (focusedWebCanvas_ && focusedWebCanvas_->isActiveInHierarchy()) {
        for (const KeyMap& key : kKeyMap) {
            if (Input::isKeyPressed(key.key)) webHandled = focusedWebCanvas_->fireKeyEvent(true, key.rml, modifiers) || true;
            if (Input::isKeyReleased(key.key)) webHandled = focusedWebCanvas_->fireKeyEvent(false, key.rml, modifiers) || webHandled;
        }
        for (uint32_t codepoint : Input::textInputCodepoints()) {
            webHandled = focusedWebCanvas_->fireTextInput(codepoint) || true;
        }
    }

    bool nativeUiActive = targetNode || hoveredNode_ || pressedNode_;
    Input::setUiCapture(focusedWebCanvas_ != nullptr || nativeUiActive,
                        webTarget != nullptr || !touchTargets_.empty() || nativeUiActive);
    return webHandled || nativeUiActive;
}

UIInteractableNode* UIInteractionSystem::raycast(UINode* node, const glm::vec2& mousePos) {
    if (!node->isActiveInHierarchy()) return nullptr;

    float globalX = node->globalX();
    float globalY = node->globalY();

    const auto& children = node->children();
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        if (UINode* uiChild = dynamic_cast<UINode*>(it->get())) {
            UIInteractableNode* hit = raycast(uiChild, mousePos);
            if (hit) return hit;
        }
    }

    if (auto interactable = dynamic_cast<UIInteractableNode*>(node)) {
        if (!interactable->interactable()) return nullptr;

        float drawX = globalX - (node->width() * node->pivotX());
        float drawY = globalY - (node->height() * node->pivotY());

        if (mousePos.x >= drawX && mousePos.x <= drawX + node->width() &&
            mousePos.y >= drawY && mousePos.y <= drawY + node->height()) {
            return interactable;
        }
    }

    return nullptr;
}

} // namespace saida
