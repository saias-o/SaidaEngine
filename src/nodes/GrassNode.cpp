#include "nodes/GrassNode.hpp"

#include "core/Log.hpp"

#include <cmath>

namespace saida {

void GrassNode::describe(reflect::TypeBuilder<GrassNode>& t) {
    t.doc("Grass blades made in the vertex shader around the camera, on a field the game supplies.");
    t.property("density", &GrassNode::density).range(1.0, 400.0)
        .tooltip("blades a square metre in the nearest ring");
    t.property("bladeHeight", &GrassNode::bladeHeight).range(0.02, 3.0).tooltip("metres");
    t.property("bladeWidth", &GrassNode::bladeWidth).range(0.002, 0.5).tooltip("metres at the root");
    t.property("radius", &GrassNode::radius).range(1.0, 200.0)
        .tooltip("metres from the camera past which no blade is drawn");
    t.property("wind", &GrassNode::wind).range(0.0, 2.0).tooltip("metres a tip sways at most");
}

bool GrassNode::setField(Field field) {
    const int n = field.groundSamples, c = field.coverSize;
    if (n < 2 || n > kMaxGroundSamples || field.heights.size() != size_t(n) * n || c < 1 || c > kMaxCoverSize ||
        field.cover.size() != size_t(c) * c) {
        Log::error("[Grass] ", name(), ": field refused: ", field.heights.size(), " heights for ", n, "^2 (2 to ",
                   kMaxGroundSamples, " a side), ", field.cover.size(), " cover texels for ", c, "^2 (at most ",
                   kMaxCoverSize, ")");
        return false;
    }
    for (float h : field.heights)
        if (!std::isfinite(h)) {
            Log::error("[Grass] ", name(), ": field refused: a height is not finite");
            return false;
        }
    field_ = std::move(field);
    present_ = true;
    ++revision_;
    return true;
}

void GrassNode::clearField() {
    field_ = Field{};
    present_ = false;
    ++revision_;
}

} // namespace saida
