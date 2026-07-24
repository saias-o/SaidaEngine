#pragma once

// AnimationCooker — compiles an authoring AnimationClip into a runtime CookedClip.
// Steps: binding tracks to rig bones (optional name-based retargeting),
// resampling cubic splines, detecting constant channels, dropping tracks
// equal to the rest pose, reducing keys under tolerance, quantization and
// paging. The result and the report are deterministic for identical inputs.

#include "scene/animation/CookedClip.hpp"
#include "scene/animation/Retarget.hpp"
#include "scene/animation/Rig.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace saida {

struct CookSettings {
    // Per-channel tolerances (bone local space): beyond this, a key is kept.
    float translationTolerance = 0.0005f;  // world units
    float rotationTolerance = 0.001f;      // radians
    float scaleTolerance = 0.0005f;
    // Resampling rate for CUBICSPLINE tracks (linearized).
    float splineResampleRate = 60.0f;
    // Maximum duration of a page: bounds the precision of quantized time
    // (page duration / 65535) and enables streaming of long sequences.
    float maxPageSeconds = 60.0f;

    uint64_t hash() const;
};

struct CookReport {
    std::string clipName;
    uint32_t sourceKeys = 0;
    uint32_t cookedKeys = 0;
    uint32_t cookedTracks = 0;
    uint32_t constantTracks = 0;
    uint32_t restPoseTracksDropped = 0;
    uint32_t unmappedTracks = 0;  // tracks with no matching bone in the rig
    size_t sourceBytes = 0;
    size_t cookedBytes = 0;
    float maxTranslationError = 0.0f;
    float maxRotationError = 0.0f;  // radians
    float maxScaleError = 0.0f;

    bool withinTolerance(const CookSettings& settings) const;
    nlohmann::json toJson() const;
};

class AnimationCooker {
public:
    // `retarget` maps rig bone names to the clip's track names (identity if
    // null). Clip tracks with no target bone are counted in the report and skipped.
    static CookedClip cook(const AnimationClip& clip, const Rig& rig,
                           const CookSettings& settings = {},
                           CookReport* report = nullptr,
                           const RetargetMap* retarget = nullptr);

    // Content hash of the (clip, rig, settings) triple: identifies the
    // derived-cache entry. Includes both the cooked-format version and the cooker version.
    static uint64_t contentHash(const AnimationClip& clip, const Rig& rig,
                                const CookSettings& settings,
                                const RetargetMap* retarget = nullptr);
};

} // namespace saida
