#include "ui/WorldPanelGeometry.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>

// World-Space UI panel geometry: a 3D panel is hit by a ray on its local z=0
// plane and the hit maps to pixel space. This is the interaction half of the
// "World Space for 3D panel" contract, proven here without a GPU or a live
// WebCanvasNode (which needs a device to exist).

using namespace saida;

namespace {

int gChecks = 0;

void require(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        std::cerr << "[ui-worldpanel] FAIL: " << what << "\n";
        std::abort();
    }
}

bool near(float a, float b, float eps = 1e-3f) {
    return std::abs(a - b) <= eps;
}

// A 2×1 world-unit panel rasterized at 800×400 pixels, centred on the origin.
constexpr float kWorldW = 2.0f;
constexpr float kWorldH = 1.0f;
constexpr uint32_t kPixW = 800;
constexpr uint32_t kPixH = 400;

// ── Identity panel: centre, corners, y-down mapping ──────────────────────────

void testCentreHit() {
    WorldPanelHit hit;
    const bool ok = raycastWorldPanel(glm::mat4(1.0f), kWorldW, kWorldH, kPixW, kPixH,
                                      {0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}, hit);
    require(ok, "ray down the -z axis hits the panel centre");
    require(near(hit.local.x, 400.0f) && near(hit.local.y, 200.0f), "centre maps to pixel (400,200)");
    require(near(hit.distance, 5.0f), "distance is the world travel to the plane");
}

void testCornersAndYDown() {
    WorldPanelHit hit;
    // Local (+halfW, +halfH) = world (1, 0.5): top-right corner, pixel (800, 0).
    require(raycastWorldPanel(glm::mat4(1.0f), kWorldW, kWorldH, kPixW, kPixH,
                              {1.0f, 0.5f, 5.0f}, {0.0f, 0.0f, -1.0f}, hit),
            "top-right corner is inside the panel");
    require(near(hit.local.x, 800.0f) && near(hit.local.y, 0.0f), "top-right maps to pixel (800,0)");

    // Local (-halfW, -halfH) = world (-1, -0.5): bottom-left, pixel (0, 400).
    require(raycastWorldPanel(glm::mat4(1.0f), kWorldW, kWorldH, kPixW, kPixH,
                              {-1.0f, -0.5f, 5.0f}, {0.0f, 0.0f, -1.0f}, hit),
            "bottom-left corner is inside the panel");
    require(near(hit.local.x, 0.0f) && near(hit.local.y, 400.0f),
            "bottom-left maps to pixel (0,400) — pixel y points down");
}

// ── Rejections: outside, parallel, behind, degenerate ────────────────────────

void testRejections() {
    WorldPanelHit hit;
    require(!raycastWorldPanel(glm::mat4(1.0f), kWorldW, kWorldH, kPixW, kPixH,
                               {2.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}, hit),
            "a ray outside the panel width misses");
    require(!raycastWorldPanel(glm::mat4(1.0f), kWorldW, kWorldH, kPixW, kPixH,
                               {0.0f, 0.0f, 5.0f}, {1.0f, 0.0f, 0.0f}, hit),
            "a ray parallel to the panel plane misses");
    require(!raycastWorldPanel(glm::mat4(1.0f), kWorldW, kWorldH, kPixW, kPixH,
                               {0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 1.0f}, hit),
            "a ray pointing away from the panel (panel behind) misses");
    require(!raycastWorldPanel(glm::mat4(1.0f), kWorldW, kWorldH, 0, kPixH,
                               {0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}, hit),
            "a zero-pixel-width panel is degenerate");
    require(!raycastWorldPanel(glm::mat4(1.0f), 0.0f, kWorldH, kPixW, kPixH,
                               {0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}, hit),
            "a zero-world-width panel is degenerate");
}

// ── Transformed panels: translation and rotation are handled ─────────────────

void testTranslatedPanel() {
    const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, -3.0f, 0.0f));
    WorldPanelHit hit;
    require(raycastWorldPanel(world, kWorldW, kWorldH, kPixW, kPixH,
                              {10.0f, -3.0f, 5.0f}, {0.0f, 0.0f, -1.0f}, hit),
            "a translated panel is hit at its moved centre");
    require(near(hit.local.x, 400.0f) && near(hit.local.y, 200.0f), "translated centre still maps to (400,200)");
    require(near(hit.distance, 5.0f), "translated distance is unchanged");
}

void testRotatedPanel() {
    // Rotate 90° about Y: the panel now faces world +x (its local +z axis).
    const glm::mat4 world = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    WorldPanelHit hit;
    require(raycastWorldPanel(world, kWorldW, kWorldH, kPixW, kPixH,
                              {5.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, hit),
            "a ray along -x hits a panel rotated to face +x");
    require(near(hit.local.x, 400.0f) && near(hit.local.y, 200.0f), "rotated-panel centre maps to (400,200)");
    require(near(hit.distance, 5.0f), "rotated-panel distance is the world travel");
}

} // namespace

int main() {
    testCentreHit();
    testCornersAndYDown();
    testRejections();
    testTranslatedPanel();
    testRotatedPanel();

    std::cout << "[ui-worldpanel] PASS (" << gChecks << " checks)\n";
    return 0;
}
