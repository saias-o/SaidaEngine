#pragma once

#include "scene/Node.hpp"
#include "core/Reflection.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace saida {

// Grass blades on a patch of ground, drawn only around the camera
// (GrassFeature). Nothing of a blade is stored: the vertex shader makes each
// one from a hash of where it grows, so a field costs two small buffers and
// no geometry at all.
//
// The caller describes the ground the blades stand on and where grass grows:
//
// * the ground is a heightfield of `groundSamples` x `groundSamples` heights
//   (node-local y), row-major, laid over the unit square of a parameter
//   (u, v): sample (i, j) at (i, j) / (groundSamples - 1). Each grid cell is
//   two triangles, (i,j)-(i+1,j)-(i+1,j+1) and (i,j)-(i+1,j+1)-(i,j+1), the
//   split a ground mesh built on the same grid draws, so a blade stands on
//   the drawn ground and not on an interpolation of it;
// * `uvFromLocal` maps node-local (x, z) to (u, v): an affine map, so the
//   patch may be any parallelogram of the node's plane;
// * the cover is `coverSize` x `coverSize` texels over the same unit square,
//   RGBA8: rgb the grass's colour (sRGB-encoded linear albedo, the colour
//   the ground under it is drawn with, so that where blades thin out with
//   distance nothing changes), a the density, 0 none to 255 the most.
//
// Blades grow in tufts of `tuftBlades`, `density` tufts a square metre near
// the camera, out to `radius` metres, thinning in three rings each twice the
// last; past the radius the ground's own colour is the grass. So that nothing
// marks where they stop, a blade's own look -- darker root, paler tip, its
// lean, its sheen, the light through it -- gives way with distance to the
// ground's shading, before the blades shorten to nothing: by then they are
// lit as the ground they hand over to, in its colour and its variation. Gusts run
// across the field along `windDirection` and lay it down as they pass.
class GrassNode : public Node {
public:
    GrassNode() : Node("Grass") {}

    SAIDA_REFLECT_NODE(GrassNode, "Grass")

    static constexpr int kMaxGroundSamples = 65;  // MAX_GROUND in grass.glsl
    static constexpr int kMaxBoundarySamples = 257; // MAX_BOUNDARY in grass.glsl
    static constexpr int kMaxCoverSize = 512;     // MAX_COVER in grass.glsl
    static constexpr int kMaxBenders = 4;         // MAX_BENDERS in grass.glsl
    static constexpr int kMaxTuftBlades = 8;

    float density = 16.0f;      // tufts a square metre, nearest ring
    int tuftBlades = 5;         // blades a tuft
    float bladeHeight = 0.45f;  // metres, at full density; each blade varies about it
    float bladeWidth = 0.06f;   // metres at the root, nearest ring
    float radius = 70.0f;       // metres from the camera past which none is drawn
    float wind = 0.35f;         // a blade's sway, in blade heights
    float gust = 0.7f;          // how hard the passing gusts lay the grass down, 0 to 1
    glm::vec2 windDirection{1.0f, 0.3f};  // node-local x, z

    struct Field {
        int groundSamples = 0;
        std::vector<float> heights;
        // Optional south/north boundary refinement: sorted (u, local y)
        // knots from u=0 to u=1, including each regular boundary vertex.
        // Each boundary triangle becomes a fan from its opposite vertex.
        std::vector<glm::vec2> southBoundary, northBoundary;
        glm::mat3 uvFromLocal{1.0f};  // (u, v, 1) = M * (x, z, 1)
        int coverSize = 0;
        std::vector<uint32_t> cover;  // RGBA8, r in the low byte
        // The ground material's surface variation (MaterialDesc::variation:
        // warp in repeats, macro cells a repeat, albedo amplitude, slope) and
        // its texture coordinates, in repeats, of node-local (x, z). The
        // blades take the brightness the ground under them is drawn with, so
        // the ground's light and dark patches do not stop where they do.
        // Zero variation: none.
        glm::mat3 materialUvFromLocal{1.0f};  // (s, t, 1) = M * (x, z, 1)
        glm::vec4 variation{0.0f};
    };
    // Replaces the field. Refused, and said, when the arrays are not the
    // documented sizes, past the limits, or a height is not finite.
    bool setField(Field field);
    void clearField();
    const Field* field() const { return present_ ? &field_ : nullptr; }
    uint64_t revision() const { return revision_; }

    // What pushes blades aside: node-local centre (xyz) and radius (w), the
    // radius 0 for none. A character or a wheel walks through the grass.
    std::array<glm::vec4, kMaxBenders> benders{};

private:
    Field field_;
    bool present_ = false;
    uint64_t revision_ = 1;
};

} // namespace saida
