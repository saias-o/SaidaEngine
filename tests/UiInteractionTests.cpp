#include "core/Camera.hpp"
#include "core/Input.hpp"
#include "scene/Scene.hpp"
#include "nodes/UIButtonNode.hpp"
#include "nodes/UICanvasNode.hpp"
#include "nodes/UITextNode.hpp"
#include "ui/UIInteractionSystem.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>

// Interaction contract for the shipping HUD path (UICanvas + UIInteractable):
// the hit-test, hover/press/click state machine and the input-capture decision
// are pure (node rects, no GPU), so they are proven headless here. Notably the
// "a HUD does not steal the game's clicks" rule: only an interactable node under
// the pointer captures; a display-only HUD leaves input to gameplay.

using namespace saida;

namespace {

int gChecks = 0;

void require(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        std::cerr << "[ui-interaction] FAIL: " << what << "\n";
        std::abort();
    }
}

// One canvas under a scene, plus helpers to add interactables at absolute
// top-left coordinates and to drive one interaction frame.
struct Fixture {
    Scene scene;
    UICanvasNode* canvas = nullptr;
    UIInteractionSystem interaction;
    Camera camera;
    glm::vec2 viewport{800.0f, 600.0f};

    Fixture() {
        Input::setUiCapture(false, false);
        canvas = static_cast<UICanvasNode*>(scene.addChild(std::make_unique<UICanvasNode>()));
    }

    UIButtonNode* addButton(float x, float y, float w, float h) {
        auto button = std::make_unique<UIButtonNode>();
        button->setAnchor(0.0f, 0.0f);  // top-left origin, no centering
        button->setPivot(0.0f, 0.0f);
        button->setPosition(x, y);
        button->setSize(w, h);
        return static_cast<UIButtonNode*>(canvas->addChild(std::move(button)));
    }

    UITextNode* addText(float x, float y, float w, float h) {
        auto text = std::make_unique<UITextNode>();
        text->setAnchor(0.0f, 0.0f);
        text->setPivot(0.0f, 0.0f);
        text->setPosition(x, y);
        text->setSize(w, h);
        return static_cast<UITextNode*>(canvas->addChild(std::move(text)));
    }

    bool frame(glm::vec2 mouse, bool down, bool justPressed, bool justReleased) {
        scene.refreshHierarchy();  // publishes uiCanvas_ once the tree changed
        return interaction.update(scene, camera, mouse, viewport, down, justPressed, justReleased);
    }
};

using State = UIInteractableNode::State;

// ── Hover follows the pointer and drives mouse capture ───────────────────────

void testHoverAndCapture() {
    Fixture fx;
    UIButtonNode* button = fx.addButton(100.0f, 100.0f, 200.0f, 60.0f);

    bool handled = fx.frame({150.0f, 130.0f}, false, false, false);
    require(handled, "pointer over a button is handled by the UI");
    require(button->state() == State::Hover, "button enters hover under the pointer");
    require(Input::uiCapturesMouse(), "hovering an interactable captures the mouse");

    handled = fx.frame({500.0f, 500.0f}, false, false, false);
    require(!handled, "pointer away from any interactable is not handled");
    require(button->state() == State::Normal, "button leaves hover when the pointer exits");
    require(!Input::uiCapturesMouse(), "capture is released when nothing is hovered");
}

// ── A press+release on the same button is a click ────────────────────────────

void testClickFires() {
    Fixture fx;
    UIButtonNode* button = fx.addButton(0.0f, 0.0f, 100.0f, 100.0f);
    int clicks = 0;
    button->setOnClick([&clicks] { ++clicks; });

    const glm::vec2 over{50.0f, 50.0f};
    fx.frame(over, false, false, false);            // hover
    fx.frame(over, true, true, false);              // press
    require(button->state() == State::Pressed, "button is pressed on mouse down");
    require(Input::uiCapturesMouse(), "a pressed button keeps mouse capture");
    fx.frame(over, false, false, true);             // release over the same node
    require(clicks == 1, "press then release on the same button fires one click");
}

// ── Releasing away from the pressed button cancels the click ─────────────────

void testClickCancelledOnDragOut() {
    Fixture fx;
    UIButtonNode* button = fx.addButton(0.0f, 0.0f, 100.0f, 100.0f);
    int clicks = 0;
    button->setOnClick([&clicks] { ++clicks; });

    fx.frame({50.0f, 50.0f}, false, false, false);  // hover
    fx.frame({50.0f, 50.0f}, true, true, false);    // press
    fx.frame({400.0f, 400.0f}, false, false, true); // release elsewhere
    require(clicks == 0, "releasing away from the pressed button cancels the click");
    require(button->state() != State::Pressed, "button leaves the pressed state after release");
}

// ── A display-only HUD must not steal the game's clicks ──────────────────────

void testHudTextDoesNotCapture() {
    Fixture fx;
    fx.addText(0.0f, 0.0f, 800.0f, 600.0f);  // full-screen HUD label

    bool handled = fx.frame({400.0f, 300.0f}, true, true, false);
    require(!handled, "a text-only HUD does not handle the click");
    require(!Input::uiCapturesMouse(), "a text-only HUD does not capture the mouse (clicks reach gameplay)");
}

// ── A disabled interactable is transparent to hit-testing ────────────────────

void testDisabledButtonIsTransparent() {
    Fixture fx;
    UIButtonNode* button = fx.addButton(0.0f, 0.0f, 200.0f, 200.0f);
    button->setInteractable(false);

    bool handled = fx.frame({100.0f, 100.0f}, false, false, false);
    require(!handled, "a disabled button does not capture the pointer");
    require(!Input::uiCapturesMouse(), "a disabled button leaves the mouse free");
}

// ── Overlapping interactables: the topmost (last child) wins ─────────────────

void testTopmostWins() {
    Fixture fx;
    UIButtonNode* under = fx.addButton(0.0f, 0.0f, 200.0f, 200.0f);
    UIButtonNode* over = fx.addButton(0.0f, 0.0f, 200.0f, 200.0f);  // added later → on top
    int underClicks = 0, overClicks = 0;
    under->setOnClick([&underClicks] { ++underClicks; });
    over->setOnClick([&overClicks] { ++overClicks; });

    const glm::vec2 shared{100.0f, 100.0f};
    fx.frame(shared, false, false, false);
    fx.frame(shared, true, true, false);
    fx.frame(shared, false, false, true);
    require(overClicks == 1 && underClicks == 0, "the topmost overlapping button receives the click");
    require(over->state() == State::Hover && under->state() == State::Normal,
            "only the topmost button reflects interaction state");
}

// ── An inactive canvas disables all interaction ──────────────────────────────

void testInactiveCanvasIgnored() {
    Fixture fx;
    UIButtonNode* button = fx.addButton(0.0f, 0.0f, 200.0f, 200.0f);
    fx.frame({100.0f, 100.0f}, false, false, false);
    require(button->state() == State::Hover, "sanity: active canvas hovers");

    fx.canvas->setEnabled(false);
    bool handled = fx.frame({100.0f, 100.0f}, false, false, false);
    require(!handled, "an inactive canvas handles nothing");
    require(button->state() == State::Normal, "hover is cleared when the canvas goes inactive");
    require(!Input::uiCapturesMouse(), "an inactive canvas releases mouse capture");
}

} // namespace

int main() {
    testHoverAndCapture();
    testClickFires();
    testClickCancelledOnDragOut();
    testHudTextDoesNotCapture();
    testDisabledButtonIsTransparent();
    testTopmostWins();
    testInactiveCanvasIgnored();

    std::cout << "[ui-interaction] PASS (" << gChecks << " checks)\n";
    return 0;
}
