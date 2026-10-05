#pragma once

#include "scene/Node.hpp"
#include "core/Reflection.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace saida {

class Material;

// Terrain to the horizon, as nested square rings around a focus (geometry
// clipmaps). Level k is a grid of kResolution x kResolution cells, each
// `baseSpacing * 2^k` metres, centred on the focus snapped to two of its
// cells; the level inside it is cut out, so each ring covers twice the extent
// of the one within. Near its outer edge a level morphs into the next one's
// sampling, so rings meet without a crack.
//
// Heights are data, not geometry: the caller samples each level where
// `levelOrigin` says (on its own threads -- a level is 129 x 129 samples) and
// hands it over with `setLevel`. The grid is generated in the vertex shader
// and the heights live in one storage buffer, so the terrain costs nothing in
// the geometry arena. A level is drawn where its data was sampled, so data
// that lags the focus is still drawn in place.
//
// Positions are node-local: x and z on the grid, y the height. A caller that
// draws a curved world returns the drop below the plane in the height.
//
// The rings shadow themselves: from every sample the renderer marches toward
// the scene's directional light over the heights it holds, finest level first
// (terrain_rings_sun.comp), again only when a level changes or the light has
// turned. A mountain darkens the valley behind it out to the horizon, at no
// cost a frame.
class TerrainRingsNode : public Node {
public:
    TerrainRingsNode() : Node("TerrainRings") {}

    SAIDA_REFLECT_NODE(TerrainRingsNode, "TerrainRings")

    static constexpr int kResolution = 128;  // cells per level side; RES in terrain_rings.vert
    static constexpr int kSamples = kResolution + 1;
    static constexpr int kMaxLevels = 12;    // MAX_LEVELS in terrain_rings.glsl
    static constexpr int kMaxLayers = 16;    // MAX_LAYERS in terrain_rings.glsl
    static constexpr int kMaxHoles = 16;     // MAX_HOLES in terrain_rings.glsl

    int levels = 9;            // rings drawn, finest first
    float baseSpacing = 16.0f; // metres between level-0 samples
    // Nothing is drawn closer than this to the focus (m): the ground the
    // caller draws itself, finer, sits there.
    float innerRadius = 0.0f;

    // One level's samples. `heights` are kSamples x kSamples, row-major, row
    // z then column x, sample (0,0) at `origin`; `layers` are one per cell,
    // kResolution x kResolution, an index into the layer palette.
    struct Level {
        glm::dvec2 origin{0.0};
        double spacing = 0.0;
        std::vector<float> heights;
        std::vector<uint8_t> layers;
    };

    // Optional PBR material, projected in node-local metres on all three axes.
    // Without a material the layer keeps its albedo/roughness. Macro variation
    // is zero by default; its scale is in metres, independent of ring spacing.
    struct Layer {
        glm::vec3 albedo{0.2f};
        float roughness = 0.9f;
        Material* material = nullptr;
        float textureScale = 1.0f;  // texture repeats per metre
        float macroSize = 128.0f;
        float macroVariation = 0.0f;
        float macroNormalStrength = 0.0f; // dimensionless slope; no displacement
    };

    // The spacing of level k, and the node-local x,z of its sample (0,0) when
    // the rings are centred on `focus`. Level k's centre is snapped to twice
    // its spacing, so its even samples are the next level's samples and its
    // square falls on the next level's cell lines.
    double levelSpacing(int k) const;
    glm::dvec2 levelOrigin(int k, glm::dvec2 focus) const;

    // Replaces level k's samples. Refused, and said, when the arrays are not
    // the documented sizes or k is out of range: returns false.
    bool setLevel(int k, Level level);
    void clearLevels();
    const Level* level(int k) const;
    // The highest sample of the levels present: past it a ray toward the
    // light meets nothing more. Lowest float when no level is.
    float highest() const;
    // Bumped by every setLevel / clearLevels: what the renderer re-uploads.
    uint64_t revision(int k) const;

    void setLayer(int index, Layer layer);
    const std::array<Layer, kMaxLayers>& layers() const { return layers_; }
    uint64_t layersRevision() const { return layersRevision_; }

    // Where the rings are centred, node-local: `innerRadius` is measured from it.
    glm::dvec2 focus{0.0};

    // Convex quadrilaterals (node-local x,z, in order around) where nothing is
    // drawn: the ground the caller draws itself, exactly where it is, so the
    // rings neither overlap it nor leave a gap beside it. Past kMaxHoles the
    // rest are ignored, and said.
    using Hole = std::array<glm::dvec2, 4>;
    void setHoles(std::vector<Hole> holes);
    const std::vector<Hole>& holes() const { return holes_; }

private:
    std::array<Level, kMaxLevels> levels_{};
    std::array<bool, kMaxLevels> present_{};
    std::array<uint64_t, kMaxLevels> revisions_{};
    std::array<float, kMaxLevels> tops_{};
    std::array<Layer, kMaxLayers> layers_{};
    uint64_t layersRevision_ = 1;
    uint64_t nextRevision_ = 1;
    std::vector<Hole> holes_;
};

} // namespace saida
