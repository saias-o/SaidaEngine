#pragma once

#include "scene/animation/AnimNode.hpp"
#include "scene/animation/AnimationClip.hpp"
#include "scene/animation/ClipView.hpp"  // ClipViewEvent
#include "scene/animation/Rig.hpp"
#include "scene/animation/Retarget.hpp"

#include <glm/glm.hpp>

#include <vector>

namespace saida {

// ClipNode plays back a single immutable AnimationClip. Pass a RetargetMap to play
// a clip whose joint names differ from the rig's (name-based retargeting); null =
// names must match.
class ClipNode : public AnimNode {
public:
    explicit ClipNode(const AnimationClip* clip, const Rig& rig,
                      const RetargetMap* retarget = nullptr);

    void update(float deltaTime) override;
    void evaluate(const LocalPose& bindPose, LocalPose& outPose) const override;

    void setPlaybackSpeed(float speed) { speed_ = speed; }
    void setLooping(bool loop) { looping_ = loop; }

    // Restricts playback (loop and clamp) to [start, end] of the shared source
    // clip — a ClipView plays a sub-range without duplicating any keys.
    void setRange(float start, float end);

    // Forcibly sets the internal playhead time.
    void setTime(float t) { time_ = t; }
    float time() const { return time_; }
    float duration() const { return clip_ ? clip_->duration() : 0.0f; }
    float rangeStart() const { return rangeStart_; }
    float rangeEnd() const;  // range end, or the clip duration without a range

    float normalizedTime() const override;
    void seekNormalized(float phase) override;

    // Events (SOURCE time, sorted): update() detects crossings, including
    // looping and reverse playback. firedEvents() lists the indices crossed
    // during the last update.
    void setEvents(std::vector<ClipViewEvent> events);
    const std::vector<ClipViewEvent>& events() const { return events_; }
    const std::vector<uint32_t>& firedEvents() const { return firedEvents_; }

    // Root motion extraction: the `boneIndex` bone's translation is replaced
    // by its bind value in the pose, and the accumulated displacement is
    // available via takeRootMotionDelta().
    void setRootMotionBone(int32_t boneIndex);
    glm::vec3 takeRootMotionDelta();

private:
    void collectCrossings(float from, float to);
    glm::vec3 rootTranslationAt(float time) const;

    const AnimationClip* clip_ = nullptr;
    float time_ = 0.0f;
    float speed_ = 1.0f;
    bool looping_ = true;
    float rangeStart_ = 0.0f;
    float rangeEnd_ = -1.0f;  // < 0 = play the whole clip

    std::vector<ClipViewEvent> events_;
    std::vector<uint32_t> firedEvents_;

    int32_t rootMotionBone_ = -1;
    glm::vec3 rootMotionDelta_{0.0f};

    // Mapping from Rig bone index to list of tracks
    std::vector<const std::vector<std::unique_ptr<AnimTrack>>*> boundTracks_;
};

} // namespace saida
