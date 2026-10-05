#include "nodes/TerrainRingsNode.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace saida {

void TerrainRingsNode::describe(reflect::TypeBuilder<TerrainRingsNode>& t) {
    t.doc("Terrain to the horizon: nested rings of a procedural grid whose heights the game supplies.");
    t.property("levels", &TerrainRingsNode::levels).range(1, kMaxLevels)
        .tooltip("rings drawn, each twice the extent of the one inside");
    t.property("baseSpacing", &TerrainRingsNode::baseSpacing).range(1.0, 1000.0)
        .tooltip("metres between the finest ring's samples");
    t.property("innerRadius", &TerrainRingsNode::innerRadius).range(0.0, 100000.0)
        .tooltip("nothing is drawn closer than this to the focus (m)");
}

double TerrainRingsNode::levelSpacing(int k) const {
    return double(baseSpacing) * std::ldexp(1.0, k);
}

glm::dvec2 TerrainRingsNode::levelOrigin(int k, glm::dvec2 focus) const {
    const double spacing = levelSpacing(k);
    const double snap = 2.0 * spacing;
    const glm::dvec2 centre(std::round(focus.x / snap) * snap, std::round(focus.y / snap) * snap);
    return centre - glm::dvec2(0.5 * kResolution * spacing);
}

bool TerrainRingsNode::setLevel(int k, Level level) {
    if (k < 0 || k >= kMaxLevels) {
        Log::error("[TerrainRings] ", name(), ": level ", k, " is past the ", kMaxLevels, " rings a node holds");
        return false;
    }
    if (level.heights.size() != size_t(kSamples) * kSamples ||
        level.layers.size() != size_t(kResolution) * kResolution || !(level.spacing > 0.0)) {
        Log::error("[TerrainRings] ", name(), ": level ", k, " refused: ", level.heights.size(), " heights and ",
                   level.layers.size(), " layers for ", kSamples, "^2 and ", kResolution, "^2, spacing ",
                   level.spacing);
        return false;
    }
    for (float h : level.heights)
        if (!std::isfinite(h)) {
            Log::error("[TerrainRings] ", name(), ": level ", k, " refused: a height is not finite");
            return false;
        }
    tops_[size_t(k)] = *std::max_element(level.heights.begin(), level.heights.end());
    levels_[size_t(k)] = std::move(level);
    present_[size_t(k)] = true;
    revisions_[size_t(k)] = ++nextRevision_;
    return true;
}

void TerrainRingsNode::clearLevels() {
    for (int k = 0; k < kMaxLevels; ++k) {
        levels_[size_t(k)] = Level{};
        present_[size_t(k)] = false;
        revisions_[size_t(k)] = ++nextRevision_;
    }
}

const TerrainRingsNode::Level* TerrainRingsNode::level(int k) const {
    if (k < 0 || k >= kMaxLevels || !present_[size_t(k)]) return nullptr;
    return &levels_[size_t(k)];
}

float TerrainRingsNode::highest() const {
    float top = std::numeric_limits<float>::lowest();
    for (int k = 0; k < kMaxLevels; ++k)
        if (present_[size_t(k)]) top = std::max(top, tops_[size_t(k)]);
    return top;
}

uint64_t TerrainRingsNode::revision(int k) const {
    return k < 0 || k >= kMaxLevels ? 0 : revisions_[size_t(k)];
}

void TerrainRingsNode::setHoles(std::vector<Hole> holes) {
    if (holes.size() > size_t(kMaxHoles)) {
        Log::warn("[TerrainRings] ", name(), ": ", holes.size(), " holes, the first ", kMaxHoles, " are cut");
        holes.resize(size_t(kMaxHoles));
    }
    holes_ = std::move(holes);
}

void TerrainRingsNode::setLayer(int index, Layer layer) {
    if (index < 0 || index >= kMaxLayers) {
        Log::error("[TerrainRings] ", name(), ": layer ", index, " is past the ", kMaxLayers, " a node holds");
        return;
    }
    layers_[size_t(index)] = layer;
    ++layersRevision_;
}

} // namespace saida
