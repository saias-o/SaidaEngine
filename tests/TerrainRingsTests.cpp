#include "nodes/TerrainRingsNode.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

// TerrainRingsNode's contract with its shader: rings that meet without a crack
// need every level's even samples to be the next level's samples and its
// square to fall on the next level's cell lines, wherever the focus is. These
// checks hold that alignment, and the refusals that keep a malformed level
// from being drawn as something plausible.

using namespace saida;

namespace {

int gChecks = 0;

void require(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        std::cerr << "[terrain-rings] FAIL: " << what << "\n";
        std::abort();
    }
}

bool whole(double v) { return std::abs(v - std::round(v)) < 1e-9; }

TerrainRingsNode::Level flatLevel(const TerrainRingsNode& node, int k, glm::dvec2 focus) {
    TerrainRingsNode::Level level;
    level.origin = node.levelOrigin(k, focus);
    level.spacing = node.levelSpacing(k);
    level.heights.assign(size_t(TerrainRingsNode::kSamples) * TerrainRingsNode::kSamples, 0.0f);
    level.layers.assign(size_t(TerrainRingsNode::kResolution) * TerrainRingsNode::kResolution, 0);
    return level;
}

void testLevelsNestOnEachOthersSamples() {
    TerrainRingsNode node;
    node.baseSpacing = 16.0f;
    const int R = TerrainRingsNode::kResolution;
    const glm::dvec2 foci[] = {{0.0, 0.0}, {123.4, -987.6}, {-35000.25, 71234.5}, {15.99, 16.01}};
    for (const glm::dvec2 focus : foci)
        for (int k = 0; k + 1 < TerrainRingsNode::kMaxLevels; ++k) {
            const double s = node.levelSpacing(k), coarse = node.levelSpacing(k + 1);
            require(coarse == 2.0 * s, "each level is twice as coarse as the one inside");
            const glm::dvec2 fine = node.levelOrigin(k, focus), outer = node.levelOrigin(k + 1, focus);
            // The fine origin is a coarse sample: even fine samples are coarse ones.
            const glm::dvec2 at = (fine - outer) / coarse;
            require(whole(at.x) && whole(at.y), "a level's even samples are the next level's samples");
            // The fine square, R fine cells, is R/2 whole coarse cells, inside the coarse grid.
            require(at.x >= 1.0 && at.y >= 1.0 && at.x + R / 2 <= R - 1 && at.y + R / 2 <= R - 1,
                    "a level's square lies inside the next level, a cell clear of its edge");
            // The focus is inside its level, well clear of the morphing edge.
            const glm::dvec2 centre = fine + glm::dvec2(0.5 * R * s);
            require(std::abs(focus.x - centre.x) <= s && std::abs(focus.y - centre.y) <= s,
                    "a level is centred on the focus to within a cell");
        }
}

void testReachIsTheHorizon() {
    TerrainRingsNode node;
    node.baseSpacing = 16.0f;
    node.levels = 9;
    const double reach = 0.5 * TerrainRingsNode::kResolution * node.levelSpacing(node.levels - 1);
    // From 4 000 m up the horizon is 226 km away (PLAN §3 I2).
    require(reach > 226000.0, "nine rings of 16 m reach past a mountaineer's horizon");
}

void testMalformedLevelsAreRefused() {
    TerrainRingsNode node;
    const glm::dvec2 focus{0.0};
    auto good = flatLevel(node, 0, focus);
    require(node.setLevel(0, good), "a well-formed level is taken");
    const uint64_t first = node.revision(0);
    require(first != 0 && node.level(0) != nullptr, "and held, with a revision");
    require(node.setLevel(0, flatLevel(node, 0, focus)) && node.revision(0) > first,
            "a new level bumps the revision");

    auto shortHeights = flatLevel(node, 1, focus);
    shortHeights.heights.pop_back();
    require(!node.setLevel(1, shortHeights) && node.level(1) == nullptr, "missing heights are refused");

    auto shortLayers = flatLevel(node, 1, focus);
    shortLayers.layers.pop_back();
    require(!node.setLevel(1, shortLayers), "missing layers are refused");

    auto nan = flatLevel(node, 1, focus);
    nan.heights[17] = std::numeric_limits<float>::quiet_NaN();
    require(!node.setLevel(1, nan), "a height that is not a number is refused");

    auto flat = flatLevel(node, 1, focus);
    flat.spacing = 0.0;
    require(!node.setLevel(1, flat), "a level without a spacing is refused");

    require(!node.setLevel(TerrainRingsNode::kMaxLevels, flatLevel(node, 0, focus)), "a level past the last is refused");

    const uint64_t before = node.revision(0);
    node.clearLevels();
    require(node.level(0) == nullptr && node.revision(0) > before, "clearing drops every level");
}

// The shadow march stops once a ray toward the light is over the highest
// sample: what the node reports must be the highest of the levels present.
void testHighestIsTheTopOfThePresentLevels() {
    TerrainRingsNode node;
    require(node.highest() == std::numeric_limits<float>::lowest(), "no level, no top");
    auto low = flatLevel(node, 0, {0.0, 0.0});
    low.heights[17] = 812.5f;
    auto high = flatLevel(node, 3, {0.0, 0.0});
    high.heights[4000] = 2043.0f;
    high.heights[4001] = -6000.0f;  // a curved world's far edge drops below the plane
    require(node.setLevel(0, low) && node.setLevel(3, high), "levels accepted");
    require(node.highest() == 2043.0f, "the highest sample of any level");
    auto lower = flatLevel(node, 3, {0.0, 0.0});
    require(node.setLevel(3, lower), "level replaced");
    require(node.highest() == 812.5f, "a replaced level's top goes with it");
    node.clearLevels();
    require(node.highest() == std::numeric_limits<float>::lowest(), "cleared, no top");
}

}  // namespace

int main() {
    testLevelsNestOnEachOthersSamples();
    testReachIsTheHorizon();
    testMalformedLevelsAreRefused();
    testHighestIsTheTopOfThePresentLevels();
    std::cout << "[terrain-rings] OK (" << gChecks << " checks)\n";
    return 0;
}
