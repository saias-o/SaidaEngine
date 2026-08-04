// How a screen-space canvas reconciles the size it was authored at with the
// window it runs in. No device is involved: the policy is arithmetic, and it
// decides both what the document lays out at and where the result lands.
#include "nodes/WebCanvasNode.hpp"

#include <cmath>
#include <cstdio>
#include <string>

using namespace saida;

namespace {

int gChecks = 0;

void check(bool ok, const std::string& what) {
    ++gChecks;
    if (!ok) {
        std::fprintf(stderr, "[webcanvas] FAILED: %s\n", what.c_str());
        std::abort();
    }
}

void checkNear(float value, float expected, float tolerance, const std::string& what) {
    if (std::fabs(value - expected) > tolerance)
        std::fprintf(stderr, "[webcanvas] %s: got %.3f, expected %.3f\n",
                     what.c_str(), value, expected);
    check(std::fabs(value - expected) <= tolerance, what);
}

// A node is not copyable, so the fixture configures one in place.
void configure(WebCanvasNode& canvas, WebCanvasNode::ScaleMode mode,
               uint32_t refW, uint32_t refH) {
    canvas.resize(1920, 1080);
    canvas.setScaleMode(mode);
    canvas.setReferenceSize(refW, refH);
}

void testStretchIsUnchanged() {
    // The historic behaviour, and still the default: the canvas becomes the
    // window, so a document laid out in absolute pixels moves with it.
    WebCanvasNode canvas;
    canvas.resize(1920, 1080);
    check(canvas.scaleMode() == WebCanvasNode::ScaleMode::Stretch, "Stretch is the default");
    canvas.applyViewportLayout({1600.0f, 900.0f});
    check(canvas.width() == 1600 && canvas.height() == 900, "Stretch resizes to the window");
    checkNear(canvas.screenPosition().x, 0.0f, 0.01f, "Stretch sits at the origin");
    checkNear(canvas.screenSize().x, 1600.0f, 0.01f, "Stretch covers the window");

    // A reference declared but no mode asking for it changes nothing.
    canvas.setReferenceSize(1920, 1080);
    canvas.applyViewportLayout({1280.0f, 1024.0f});
    check(canvas.width() == 1280 && canvas.height() == 1024,
          "a reference alone does not change Stretch");
}

void testFitKeepsOneResolution() {
    WebCanvasNode canvas;
    configure(canvas, WebCanvasNode::ScaleMode::Fit, 1920, 1080);

    // Same aspect: the document keeps its size and fills the window exactly.
    canvas.applyViewportLayout({1600.0f, 900.0f});
    check(canvas.width() == 1920 && canvas.height() == 1080,
          "Fit lays out at the reference size whatever the window");
    checkNear(canvas.screenSize().x, 1600.0f, 0.5f, "Fit scales to the window width");
    checkNear(canvas.screenSize().y, 900.0f, 0.5f, "Fit scales to the window height");
    checkNear(canvas.screenPosition().x, 0.0f, 0.5f, "no bars at the same aspect");

    // Taller window: bars above and below, the document still 1920x1080.
    canvas.applyViewportLayout({1280.0f, 1024.0f});
    check(canvas.width() == 1920 && canvas.height() == 1080,
          "Fit still lays out at the reference size at 4:3");
    const float scale = 1280.0f / 1920.0f;
    checkNear(canvas.screenSize().x, 1280.0f, 0.5f, "Fit uses the full width at 4:3");
    checkNear(canvas.screenSize().y, 1080.0f * scale, 0.5f, "Fit keeps the aspect at 4:3");
    checkNear(canvas.screenPosition().x, 0.0f, 0.5f, "no horizontal bar at 4:3");
    checkNear(canvas.screenPosition().y, (1024.0f - 1080.0f * scale) * 0.5f, 0.5f,
              "the letterbox is centred");

    // Wider window: bars left and right.
    canvas.applyViewportLayout({2560.0f, 1080.0f});
    checkNear(canvas.screenSize().y, 1080.0f, 0.5f, "Fit uses the full height at 21:9");
    checkNear(canvas.screenPosition().x, (2560.0f - 1920.0f) * 0.5f, 0.5f,
              "the pillarbox is centred");
}

void testExpandFillsWithoutBars() {
    WebCanvasNode canvas;
    configure(canvas, WebCanvasNode::ScaleMode::Expand, 1920, 1080);

    // A wider window keeps the reference height and gains logical width, so the
    // pixel scale still matches the reference and nothing is letterboxed.
    canvas.applyViewportLayout({2560.0f, 1080.0f});
    check(canvas.height() == 1080, "Expand keeps the reference on the constrained axis");
    check(canvas.width() == 2560, "Expand gains logical room on the other");
    checkNear(canvas.screenPosition().x, 0.0f, 0.5f, "Expand leaves no bars");
    checkNear(canvas.screenSize().x, 2560.0f, 0.5f, "Expand covers the window");

    // A taller window is the same story rotated.
    canvas.applyViewportLayout({1280.0f, 1024.0f});
    const float scale = 1280.0f / 1920.0f;
    check(canvas.width() == 1920, "Expand keeps the reference width when height constrains");
    checkNear(static_cast<float>(canvas.height()), 1024.0f / scale, 1.5f,
              "Expand gains logical height");
}

void testMissingReferenceFallsBackToStretch() {
    // Declaring a mode without a reference has nothing to scale against. It
    // behaves as Stretch rather than dividing by zero or freezing at a size.
    WebCanvasNode canvas;
    configure(canvas, WebCanvasNode::ScaleMode::Fit, 0, 0);
    canvas.applyViewportLayout({1600.0f, 900.0f});
    check(canvas.width() == 1600 && canvas.height() == 900,
          "Fit without a reference stretches");
    checkNear(canvas.screenSize().x, 1600.0f, 0.5f, "and covers the window");
}

void testPlacementDrivesPointerMapping() {
    // Rendering and hit-testing read the same placement, so a click inside the
    // letterboxed image lands where it looks like it should, and one on a bar
    // misses the canvas entirely.
    WebCanvasNode canvas;
    configure(canvas, WebCanvasNode::ScaleMode::Fit, 1920, 1080);
    canvas.applyViewportLayout({1280.0f, 1024.0f});

    const float scale = 1280.0f / 1920.0f;
    const float barHeight = (1024.0f - 1080.0f * scale) * 0.5f;

    check(!canvas.screenContains({640.0f, barHeight * 0.5f}),
          "a point on the letterbox bar is outside the canvas");
    check(canvas.screenContains({640.0f, 512.0f}), "the centre is inside the canvas");

    const glm::vec2 centre = canvas.screenToLocal({640.0f, 512.0f});
    checkNear(centre.x, 960.0f, 1.0f, "the window centre maps to the document centre");
    checkNear(centre.y, 540.0f, 1.0f, "vertically too");

    const glm::vec2 topLeft = canvas.screenToLocal({0.0f, barHeight});
    checkNear(topLeft.x, 0.0f, 1.0f, "the image's top-left maps to the document origin");
    checkNear(topLeft.y, 0.0f, 1.0f, "vertically too");
}

void testAuthoredTransformStillDecidesWhoFills() {
    // The placement must not be mistaken for the authored intent: a canvas that
    // has been letterboxed once still asks to fill the viewport, or it would
    // freeze at the first window size it ever saw.
    WebCanvasNode canvas;
    configure(canvas, WebCanvasNode::ScaleMode::Fit, 1920, 1080);
    check(canvas.fillsViewport(), "a canvas at the origin with unit scale fills the viewport");
    canvas.applyViewportLayout({1280.0f, 1024.0f});
    check(canvas.fillsViewport(), "and still does once letterboxed");

    canvas.transform().position = {40.0f, 20.0f, 0.0f};
    check(!canvas.fillsViewport(), "a canvas placed away from the origin does not");
}

}  // namespace

int main() {
    testStretchIsUnchanged();
    testFitKeepsOneResolution();
    testExpandFillsWithoutBars();
    testMissingReferenceFallsBackToStretch();
    testPlacementDrivesPointerMapping();
    testAuthoredTransformStillDecidesWhoFills();
    std::printf("PASS: web canvas scale modes (%d checks)\n", gChecks);
    return 0;
}
