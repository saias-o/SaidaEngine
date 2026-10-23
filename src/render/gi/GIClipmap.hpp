#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/type_precision.hpp>
#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace saida {

constexpr int kGIMaxCascades = 6;
constexpr int kGIBrickSize = 8;

struct GIConfig {
    glm::ivec3 probes{16, 8, 16};
    glm::ivec3 voxels{64, 32, 64};
    int cascades = 1;
    float spacing = 1.2f;
    int probeBudget = 192, brickBudget = 16;
    // While probes wait for their first trace (start, teleport, a cascade
    // scrolling in), each frame takes up to this many budgets: the camera
    // arrives somewhere and its light settles in a fraction of the time.
    int catchUp = 3;
    float lookAheadSeconds = .75f;
    int maxProbeBudget() const { return probeBudget * catchUp; }
    bool follow = false;
    int probesPerCascade() const { return probes.x * probes.y * probes.z; }
    int probeCount() const { return probesPerCascade() * cascades; }
    glm::ivec3 bricks() const { return voxels / kGIBrickSize; }
    int bricksPerCascade() const { const auto b = bricks(); return b.x * b.y * b.z; }
};

// Owns the toroidal grids and fair work queues: scrolling preserves storage for
// retained world cells, and every frame obeys both independent work budgets.
class GIClipmap {
public:
    struct Cascade {
        glm::i64vec3 cell{0};
        glm::dvec3 origin{0};
        glm::ivec3 probeWrap{0}, voxelWrap{0};
        double spacing = 1;
        bool initialized = false;
    };
    explicit GIClipmap(GIConfig config);
    // A caller without a frame clock can leave deltaSeconds at zero (no prediction).
    void move(glm::dvec3 camera, double deltaSeconds = 0);
    void invalidate(glm::dvec3 low, glm::dvec3 high);
    void plan();
    const GIConfig& config() const { return config_; }
    const Cascade& cascade(int k) const { return cascades_[k]; }
    // x = global storage index, y = cascade, z = discard old probe history.
    const std::vector<glm::ivec4>& probes() const { return plannedProbes_; }
    const std::vector<uint32_t>& bricks() const { return plannedBricks_; }
    const std::vector<uint32_t>& invalidProbes() const { return invalidProbes_; }
    const std::vector<uint32_t>& brickMask() const { return brickMask_; }
    float warmupProgress() const;
    int queuedBricks() const;
    static glm::ivec3 wrap(glm::i64vec3 cell, glm::ivec3 size);
    static int index(glm::ivec3 c, glm::ivec3 size) { return c.x + size.x * (c.y + size.y*c.z); }
    static glm::ivec3 coord(int i, glm::ivec3 size) { return {i%size.x, (i/size.x)%size.y, i/(size.x*size.y)}; }
private:
    void dirtyProbe(int k, int i, bool reset);
    void dirtyBrick(int k, int i, bool entering);
    void voxelRegion(int k, glm::dvec3 low, glm::dvec3 high, bool entering = false);
    GIConfig config_;
    std::array<Cascade, kGIMaxCascades> cascades_;
    std::array<std::deque<int>, kGIMaxCascades> probeQueue_, brickQueue_;
    std::vector<uint8_t> probeDirty_, probeReset_, brickDirty_, ready_, selected_, brickEntering_;
    std::vector<uint32_t> invalidProbes_, plannedBricks_, brickMask_;
    std::vector<glm::ivec4> plannedProbes_;
    std::array<int, kGIMaxCascades> probeCursor_{}, enteringBricks_{};
    glm::dvec3 previousCamera_{0}, velocity_{0};
    bool hasCamera_ = false;
    int untraced_ = 0;  // probes whose ready_ is 0
    uint64_t probeTurn_ = 0, brickTurn_ = 0;
};
}
