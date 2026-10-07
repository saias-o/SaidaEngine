#include "nodes/GrassNode.hpp"
#include "render/features/GrassFeature.hpp"
#include "scene/Scene.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

// GrassNode's contract: a malformed field is refused rather than drawn as
// something plausible, and the rings GrassFeature draws around the camera
// nest on each other's cells, each square cut out of the next exactly, so no
// band of ground is drawn twice or left bare.

using namespace saida;

namespace {

int gChecks = 0;

void require(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        std::cerr << "[grass] FAIL: " << what << "\n";
        std::abort();
    }
}

bool whole(float v) { return std::abs(v - std::round(v)) < 1e-3f; }

GrassNode::Field flatField(int ground, int cover) {
    GrassNode::Field f;
    f.groundSamples = ground;
    f.heights.assign(size_t(ground) * ground, 0.0f);
    f.coverSize = cover;
    f.cover.assign(size_t(cover) * cover, 0xff3a5a2au);
    return f;
}

void testFieldsAreCheckedBeforeTheyAreDrawn() {
    GrassNode node;
    require(node.field() == nullptr, "a new node has no field");
    const uint64_t before = node.revision();
    require(node.setField(flatField(41, 256)), "a well-formed field is taken");
    require(node.field() != nullptr && node.revision() != before, "taking a field changes the revision");
    require(!node.setField(flatField(GrassNode::kMaxGroundSamples + 1, 16)), "too many ground samples are refused");
    require(!node.setField(flatField(41, GrassNode::kMaxCoverSize + 1)), "too large a cover is refused");
    GrassNode::Field short_ = flatField(41, 64);
    short_.heights.pop_back();
    require(!node.setField(short_), "a field missing a height is refused");
    GrassNode::Field nan = flatField(5, 4);
    nan.heights[3] = std::nanf("");
    require(!node.setField(nan), "a height that is not finite is refused");
    GrassNode::Field varied = flatField(5, 4);
    varied.variation = glm::vec4(1.5f, 0.02f, 0.3f, 0.0f);
    varied.materialUvFromLocal = glm::mat3(glm::vec3(0.7f, 0.0f, 0.0f), glm::vec3(0.0f, -0.7f, 0.0f),
                                           glm::vec3(0.0f, 0.0f, 1.0f));
    GrassNode probe;
    require(probe.setField(varied), "a field carrying its ground's variation is taken");
    varied.variation.z = std::nanf("");
    require(!probe.setField(varied), "a ground variation that is not finite is refused");
    varied.variation.z = 0.3f;
    varied.materialUvFromLocal[2][0] = INFINITY;
    require(!probe.setField(varied), "ground texture coordinates that are not finite are refused");
    require(node.field() != nullptr && node.field()->groundSamples == 41, "a refusal keeps the field there was");
    node.clearField();
    require(node.field() == nullptr, "a cleared node has no field");
}

void testRingsNestExactly() {
    const glm::vec2 lo(-500.0f), hi(500.0f);
    for (const glm::vec2 eye : {glm::vec2(0.0f), glm::vec2(13.37f, -7.21f), glm::vec2(-123.4f, 77.7f)}) {
        GrassFeature::RingDraw rings[GrassFeature::kRings];
        for (int k = 0; k < GrassFeature::kRings; ++k)
            rings[k] = GrassFeature::ringDraw(k, 48.0f, 1000.0f, eye, lo, hi);
        for (int k = 0; k < GrassFeature::kRings; ++k) {
            const auto& r = rings[k];
            require(r.cells.x == GrassFeature::kRingCells && r.cells.y == GrassFeature::kRingCells,
                    "a ring inside the field is drawn whole");
            require(whole(r.origin.x / r.spacing) && whole(r.origin.y / r.spacing),
                    "a ring's cells are the world's, wherever the camera");
            if (k == 0) { require(r.hole == glm::vec2(0.0f), "the nearest ring has no hole"); continue; }
            const auto& inner = rings[k - 1];
            require(std::abs(inner.spacing * 2.0f - r.spacing) < 1e-5f, "each ring twice as sparse");
            const glm::vec2 centre = r.origin + 0.5f * float(GrassFeature::kRingCells) * r.spacing;
            const glm::vec2 innerCentre = inner.origin + 0.5f * float(GrassFeature::kRingCells) * inner.spacing;
            require(glm::length(centre - innerCentre) < 1e-3f, "the rings share a centre");
            require(std::abs(r.hole.x - 0.5f * float(GrassFeature::kRingCells) * inner.spacing) < 1e-3f,
                    "a ring's hole is the ring inside, exactly");
            require(whole(r.hole.x / r.spacing), "the hole falls on the ring's cell lines");
        }
        require(glm::length(eye - (rings[0].origin + 0.5f * float(GrassFeature::kRingCells) * rings[0].spacing)) <
                    2.0f * rings[GrassFeature::kRings - 1].spacing,
                "the rings stay centred on the camera");
    }
}

void testOnlyTheFieldAndTheReachAreDrawn() {
    // A field 10 m wide, the camera 30 m away with a 36 m reach: a sliver.
    const auto near = GrassFeature::ringDraw(2, 48.0f, 36.0f, glm::vec2(30.0f, 0.0f), glm::vec2(-10.0f, -5.0f),
                                             glm::vec2(0.0f, 5.0f));
    require(near.cells.x > 0 && near.cells.y > 0, "a field within reach is drawn");
    require(float(near.cells.x) * near.spacing < 12.0f && float(near.cells.y) * near.spacing < 12.0f,
            "only the cells over the field are drawn");
    const auto far = GrassFeature::ringDraw(0, 48.0f, 36.0f, glm::vec2(200.0f, 0.0f), glm::vec2(-10.0f),
                                            glm::vec2(10.0f));
    require(far.cells.x == 0 || far.cells.y == 0, "a field out of reach draws nothing");
}

// The renderer draws the fields the scene lists: a grass node in the tree
// must be in that list, and leave it when it is disabled or hidden. Missing
// from it, every field was culled before the GPU saw it, without a word.
void testTheSceneListsItsFields() {
    Scene scene;
    Node* tile = scene.createChild<Node>();
    auto* grass = tile->createChild<GrassNode>();
    scene.update(0.01f);
    require(scene.grassFields().size() == 1 && scene.grassFields().front() == grass,
            "a grass node in the tree is listed for the renderer");
    grass->setVisible(false);
    scene.refreshHierarchy();
    require(scene.grassFields().empty(), "a hidden grass node is not listed");
    grass->setVisible(true);
    tile->setEnabled(false);
    scene.update(0.01f);
    require(scene.grassFields().empty(), "a grass node under a disabled tile is not listed");
    tile->setEnabled(true);
    scene.update(0.01f);
    require(scene.grassFields().size() == 1, "enabled again, it is listed again");
}

}  // namespace

int main() {
    testFieldsAreCheckedBeforeTheyAreDrawn();
    testRingsNestExactly();
    testOnlyTheFieldAndTheReachAreDrawn();
    testTheSceneListsItsFields();
    std::cout << "[grass] " << gChecks << " checks passed\n";
    return 0;
}
