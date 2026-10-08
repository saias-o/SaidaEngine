#include "nodes/GrassNode.hpp"

#include "core/Log.hpp"

#include <cmath>
#include <algorithm>

namespace saida {

void GrassNode::describe(reflect::TypeBuilder<GrassNode>& t) {
    t.doc("Grass blades made in the vertex shader around the camera, on a field the game supplies.");
    t.property("density", &GrassNode::density).range(1.0, 100.0)
        .tooltip("tufts a square metre in the nearest ring");
    t.property("tuftBlades", &GrassNode::tuftBlades).range(1, kMaxTuftBlades).tooltip("blades a tuft");
    t.property("bladeHeight", &GrassNode::bladeHeight).range(0.02, 3.0).tooltip("metres");
    t.property("bladeWidth", &GrassNode::bladeWidth).range(0.002, 0.5).tooltip("metres at the root");
    t.property("radius", &GrassNode::radius).range(1.0, 200.0)
        .tooltip("metres from the camera past which no blade is drawn");
    t.property("wind", &GrassNode::wind).range(0.0, 2.0).tooltip("a blade's sway, in blade heights");
    t.property("gust", &GrassNode::gust).range(0.0, 1.0)
        .tooltip("how hard the gusts running across the field lay it down");
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
    for (const auto* edge : {&field.southBoundary, &field.northBoundary}) {
        if (edge->empty()) continue;
        bool valid = edge->size() >= size_t(n) && edge->size() <= kMaxBoundarySamples &&
                     edge->front().x == 0.f && edge->back().x == 1.f;
        for (size_t i=0;i<edge->size();++i) {
            const auto p=(*edge)[i];
            valid &= std::isfinite(p.x) && std::isfinite(p.y) && p.x >= 0.f && p.x <= 1.f &&
                     (i==0 || p.x > (*edge)[i-1].x);
        }
        const size_t row = edge == &field.southBoundary ? 0 : size_t(n-1)*n;
        for (int col=0;valid && col<n;++col) {
            const float u=float(col)/float(n-1);
            auto at=std::lower_bound(edge->begin(),edge->end(),u,[](const glm::vec2& p,float x){return p.x<x;});
            if(at==edge->end())--at;
            else if(at!=edge->begin() && std::abs((at-1)->x-u)<std::abs(at->x-u))--at;
            valid = std::abs(at->x-u)<=1e-6f && std::abs(at->y-field.heights[row+size_t(col)])<=1e-3f;
        }
        if (!valid) {
            Log::error("[Grass] ", name(), ": field refused: invalid boundary knots or a boundary vertex disagrees");
            return false;
        }
    }
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(field.variation[i]) || (i < 3 && !std::isfinite(field.materialUvFromLocal[i][0] +
                                                                           field.materialUvFromLocal[i][1]))) {
            Log::error("[Grass] ", name(), ": field refused: its ground variation is not finite");
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
