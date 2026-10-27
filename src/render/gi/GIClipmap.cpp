#include "render/gi/GIClipmap.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saida {
GIClipmap::GIClipmap(GIConfig config) : config_(config),
    probeDirty_(config.probeCount()), probeReset_(config.probeCount()),
    brickDirty_(config.bricksPerCascade()*config.cascades), ready_(config.probeCount()),
    selected_(config.probeCount()), brickEntering_(brickDirty_.size()),
    brickMask_(brickDirty_.size()), untraced_(config.probeCount()) {
    if (config.cascades < 1 || config.cascades > kGIMaxCascades ||
        glm::any(glm::lessThan(config.probes, glm::ivec3(2))) ||
        glm::any(glm::notEqual(config.voxels % kGIBrickSize, glm::ivec3(0))) ||
        !(config.spacing > 0) || config.probeBudget < config.cascades || config.brickBudget < config.cascades ||
        config.catchUp < 1 || !std::isfinite(config.lookAheadSeconds) || config.lookAheadSeconds < 0)
        throw std::invalid_argument("invalid GI clipmap configuration");
}
glm::ivec3 GIClipmap::wrap(glm::i64vec3 cell, glm::ivec3 size) {
    glm::ivec3 result;
    for (int a=0; a<3; ++a) result[a] = int((cell[a] % size[a] + size[a]) % size[a]);
    return result;
}
void GIClipmap::dirtyProbe(int k, int i, bool reset) {
    const int global = k*config_.probesPerCascade()+i;
    if (!probeDirty_[global]) { probeDirty_[global]=1; probeQueue_[k].push_back(i); }
    if (reset && !probeReset_[global]) {
        probeReset_[global]=1; untraced_+=ready_[global]; ready_[global]=0; invalidProbes_.push_back(uint32_t(global));
    }
}
void GIClipmap::dirtyBrick(int k, int i, bool entering) {
    const int global = k*config_.bricksPerCascade()+i;
    if (!brickDirty_[global]) { brickDirty_[global]=1; brickQueue_[k].push_back(i); }
    if (entering && !brickEntering_[global]) { brickEntering_[global]=1; ++enteringBricks_[k]; }
}
void GIClipmap::voxelRegion(int k, glm::dvec3 low, glm::dvec3 high, bool entering) {
    const auto& c=cascades_[k];
    const glm::dvec3 extent=glm::dvec3(config_.probes)*c.spacing;
    low=glm::max(low,c.origin); high=glm::min(high,c.origin+extent);
    if (glm::any(glm::greaterThanEqual(low,high))) return;
    const auto voxelSize=extent/glm::dvec3(config_.voxels);
    const glm::i64vec3 a=glm::i64vec3(glm::floor(low/voxelSize/double(kGIBrickSize)));
    const glm::i64vec3 b=glm::i64vec3(glm::floor((high-glm::dvec3(1e-7))/voxelSize/double(kGIBrickSize)));
    for (int64_t z=a.z; z<=b.z; ++z) for (int64_t y=a.y; y<=b.y; ++y) for (int64_t x=a.x; x<=b.x; ++x)
        dirtyBrick(k,index(wrap({x,y,z},config_.bricks()),config_.bricks()),entering);
}
void GIClipmap::move(glm::dvec3 camera, double deltaSeconds) {
    invalidProbes_.clear();
    constexpr double kVelocityResponseSeconds = .15;
    const auto step=camera-previousCamera_;
    const double teleportDistance=config_.spacing*double(std::min(config_.probes.x,config_.probes.z))*.5;
    if(hasCamera_ && deltaSeconds>0 && std::isfinite(deltaSeconds) && glm::length(step)<teleportDistance)
        velocity_+=(step/deltaSeconds-velocity_)*(1.0-std::exp(-deltaSeconds/kVelocityResponseSeconds));
    else velocity_=glm::dvec3(0);
    previousCamera_=camera;hasCamera_=true;
    for (int k=0; k<config_.cascades; ++k) {
        auto& c=cascades_[k]; c.spacing=std::ldexp(double(config_.spacing),k);
        const auto old=c.cell;
        // Leave at least a quarter of the grid behind the eye on every axis:
        // turning around still has retained coverage, even at vehicle speeds.
        const auto margin=glm::max(glm::dvec3(0),glm::min(glm::dvec3(config_.probes)*.25,
            glm::dvec3(config_.probes/2)-glm::dvec3(2)))*c.spacing;
        const auto focus=camera+glm::clamp(velocity_*double(config_.lookAheadSeconds),-margin,margin);
        glm::i64vec3 desired=-glm::i64vec3(config_.probes/2);
        if (config_.follow) desired=glm::i64vec3(glm::floor(focus/c.spacing))-glm::i64vec3(config_.probes/2);
        if (c.initialized) {
            if (!config_.follow) continue;
            const auto offset=focus/c.spacing-(glm::dvec3(old)+glm::dvec3(config_.probes)*.5);
            for (int a=0;a<3;++a) if (std::abs(offset[a])<1.0) desired[a]=old[a];
            if (desired==old) continue;
        }
        const bool first=!c.initialized;
        c.cell=desired; c.origin=glm::dvec3(desired)*c.spacing;
        c.probeWrap=wrap(desired,config_.probes);
        c.voxelWrap=wrap(desired*glm::i64vec3(config_.voxels/config_.probes),config_.voxels);
        c.initialized=true;
        for (int i=0;i<config_.probesPerCascade();++i) {
            const auto g=desired+glm::i64vec3(coord(i,config_.probes));
            if (first || glm::any(glm::lessThan(g,old)) || glm::any(glm::greaterThanEqual(g,old+glm::i64vec3(config_.probes))))
                dirtyProbe(k,index(wrap(g,config_.probes),config_.probes),true);
        }
        const auto extent=glm::dvec3(config_.probes)*c.spacing;
        if (first || glm::any(glm::greaterThanEqual(glm::abs(desired-old),glm::i64vec3(config_.probes)))) {
            voxelRegion(k,c.origin,c.origin+extent,true);
        } else {
            const auto oldOrigin=glm::dvec3(old)*c.spacing;
            for(int a=0;a<3;++a) if(desired[a]!=old[a]) {
                auto low=c.origin,high=c.origin+extent;
                if(desired[a]>old[a]) low[a]=oldOrigin[a]+extent[a];
                else high[a]=oldOrigin[a];
                voxelRegion(k,low,high,true);
            }
        }
        // Streaming edits may already occupy the queues. Recycled storage must
        // be filled and traced before that maintenance work, while still far away.
        std::stable_partition(brickQueue_[k].begin(),brickQueue_[k].end(),
            [&](int i) { return brickEntering_[k*config_.bricksPerCascade()+i]!=0; });
        std::stable_partition(probeQueue_[k].begin(),probeQueue_[k].end(),
            [&](int i) { return probeReset_[k*config_.probesPerCascade()+i]!=0; });
    }
}
void GIClipmap::invalidate(glm::dvec3 low, glm::dvec3 high) {
    for(int k=0;k<config_.cascades;++k) {
        const auto& c=cascades_[k]; if(!c.initialized) continue;
        voxelRegion(k,low,high);
        auto a=glm::i64vec3(glm::floor(low/c.spacing))-glm::i64vec3(1);
        auto b=glm::i64vec3(glm::ceil(high/c.spacing))+glm::i64vec3(1);
        a=glm::max(a,c.cell); b=glm::min(b,c.cell+glm::i64vec3(config_.probes)-glm::i64vec3(1));
        for(int64_t z=a.z;z<=b.z;++z) for(int64_t y=a.y;y<=b.y;++y) for(int64_t x=a.x;x<=b.x;++x)
            dirtyProbe(k,index(wrap({x,y,z},config_.probes),config_.probes),false);
    }
}
void GIClipmap::plan() {
    plannedBricks_.clear(); plannedProbes_.clear();
    std::fill(brickMask_.begin(),brickMask_.end(),0);
    std::fill(selected_.begin(),selected_.end(),0);
    // A minimum share for each cascade; fine cascades receive the spare turns.
    const int cycle=config_.cascades+2;
    auto cascadeFor=[&](uint64_t turn) { const int i=int(turn%cycle); return i<config_.cascades ? i : (i-config_.cascades)%config_.cascades; };
    // Optimized profiles voxelize one cascade per frame. The maximum-quality
    // profile drains every cascade before tracing, including after a teleport.
    for(int attempts=0;attempts<cycle;++attempts) {
        const int k=cascadeFor(brickTurn_++); auto& q=brickQueue_[k];
        if(q.empty()) continue;
        while(!q.empty() && int(plannedBricks_.size())<config_.brickBudget) {
            const int i=q.front();q.pop_front();const int global=k*config_.bricksPerCascade()+i;
            brickDirty_[global]=0; brickMask_[global]=1; plannedBricks_.push_back(uint32_t(global));
            if(brickEntering_[global]) {brickEntering_[global]=0;--enteringBricks_[k];}
        }
        if(!config_.updateAll) break;
    }
    const int budget=untraced_>0?config_.maxProbeBudget():config_.probeBudget;
    for(int attempts=0;int(plannedProbes_.size())<budget && attempts<budget*cycle;++attempts) {
        const int k=cascadeFor(probeTurn_++);
        // Initial fills AND entering slabs contain recycled voxel storage.
        // Ordinary edits never gate probes: streaming keeps those queues busy.
        if(enteringBricks_[k]>0) continue;
        int i;
        if(!probeQueue_[k].empty()) { i=probeQueue_[k].front();probeQueue_[k].pop_front(); }
        else { i=probeCursor_[k]++%config_.probesPerCascade(); }
        const int global=k*config_.probesPerCascade()+i;
        probeDirty_[global]=0;
        if(selected_[global]) continue;
        selected_[global]=1;
        plannedProbes_.push_back({global,k,probeReset_[global],0});
        probeReset_[global]=0; untraced_-=1-ready_[global]; ready_[global]=1;
    }
}
int GIClipmap::queuedBricks() const {
    int total=0; for(int k=0;k<config_.cascades;++k) total+=int(brickQueue_[k].size()); return total;
}
float GIClipmap::warmupProgress() const {
    return 1.0f-float(untraced_)/float(ready_.size());
}
}
