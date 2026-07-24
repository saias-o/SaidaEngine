#pragma once

// AnimationSequence — deterministic multi-track montage (.sseq, JSON). An
// animation track targets a character (clips placed on the timeline with
// trim, speed, loop, and in/out blends), an event track emits signals, a
// property track animates a reflected value through the existing Timeline.
// The runtime never reads the JSON directly: SequencePlayer compiles the
// sequence into animation nodes injected into the target Animators.
//
// JSON schema (schema == kAnimationSequenceSchema):
//   {
//     "schema": 1,
//     "name": "Intro",
//     "duration": 10.0,
//     "tracks": [
//       { "type": "animation", "target": "Hero", "clips": [
//           { "start": 0.0, "duration": 4.0, "clip": "hero.glb#Walk",
//             "trimStart": 0.5, "speed": 1.0, "loop": true,
//             "blendIn": 0.25, "blendOut": 0.25 } ] },
//       { "type": "event", "events": [ { "time": 2.0, "name": "door_open" } ] },
//       { "type": "property", "target": "sun.intensity", "keys": [
//           { "time": 0.0, "value": 1.0 }, { "time": 3.0, "value": 0.2 } ] }
//     ]
//   }

#include "core/Signal.hpp"
#include "scene/animation/AnimNode.hpp"
#include "scene/animation/ClipView.hpp"  // AssetDiagnostic, ClipViewEvent
#include "scene/animation/Timeline.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace saida {

class AnimationClip;
class Animator;
class ClipNode;
class Rig;

constexpr int kAnimationSequenceSchema = 1;

struct SequenceClipEntry {
    float start = 0.0f;      // seconds on the timeline
    float duration = 0.0f;   // duration occupied on the timeline
    std::string clip;        // sub-asset key "file#clip"
    float trimStart = 0.0f;  // offset into the source
    float speed = 1.0f;
    bool loop = false;
    float blendIn = 0.0f;
    float blendOut = 0.0f;

    float end() const { return start + duration; }
    // Blend weight [0,1] at `time` (linear in/out ramps).
    float weightAt(float time) const;
};

struct SequenceAnimationTrack {
    std::string target;  // logical character name (resolved by the player)
    std::vector<SequenceClipEntry> clips;
};

struct SequencePropertyKey {
    float time = 0.0f;
    nlohmann::json value;
};

struct SequencePropertyTrack {
    std::string target;  // property path, resolved by the player's binder
    std::vector<SequencePropertyKey> keys;
};

struct AnimationSequenceParseResult;

class AnimationSequence {
public:
    static AnimationSequenceParseResult parse(const nlohmann::json& j);
    static AnimationSequenceParseResult loadFile(const std::string& path);

    nlohmann::json toJson() const;
    bool saveFile(const std::string& path) const;

    // Internal consistency: positive durations, clips within the timeline,
    // blends shorter than their clip, events within the sequence (warning).
    std::vector<AssetDiagnostic> validate() const;

    std::string name;
    float duration = 0.0f;
    std::vector<SequenceAnimationTrack> animationTracks;
    std::vector<ClipViewEvent> events;
    std::vector<SequencePropertyTrack> propertyTracks;
};

struct AnimationSequenceParseResult {
    bool ok = false;
    AnimationSequence sequence;
    std::vector<AssetDiagnostic> diagnostics;
};

// Runtime node for an animation track: resolved clips are sampled at the
// sequence time (deterministic seek) and blended by their ramps.
class SequenceTrackNode : public AnimNode {
public:
    SequenceTrackNode(const SequenceAnimationTrack& track,
                      const std::function<const AnimationClip*(const std::string&)>& resolveClip,
                      const Rig& rig, float sequenceDuration,
                      std::vector<AssetDiagnostic>* diagnostics = nullptr);

    void update(float deltaTime) override;
    void evaluate(const LocalPose& bindPose, LocalPose& outPose) const override;

    void setTime(float time);
    float time() const { return time_; }
    bool empty() const { return entries_.empty(); }

private:
    struct Entry {
        SequenceClipEntry placement;
        std::unique_ptr<ClipNode> node;
    };

    float localClipTime(const SequenceClipEntry& placement, float clipDuration) const;

    std::vector<Entry> entries_;
    float duration_ = 0.0f;
    float time_ = 0.0f;

    mutable LocalPose scratchPose_;
};

// Drives an AnimationSequence: injects a SequenceTrackNode into each target
// Animator, connects property tracks to the reflected Timeline, and emits
// events during playback. seek() is deterministic and silent.
class SequencePlayer {
public:
    using ClipResolver = std::function<const AnimationClip*(const std::string& key)>;
    using AnimatorResolver = std::function<Animator*(const std::string& target)>;
    // The binder attaches a property track to its reflected target;
    // returning false leaves the track unbound (diagnostic).
    using PropertyBinder =
        std::function<bool(const std::string& target, TimelinePropertyTrack& track)>;

    bool bind(const AnimationSequence& sequence, const AnimatorResolver& resolveAnimator,
              const ClipResolver& resolveClip, const PropertyBinder& bindProperty = nullptr,
              std::vector<AssetDiagnostic>* diagnostics = nullptr);

    void seek(float time);       // positions everything, emits no events
    void update(float deltaTime);  // advances, evaluates properties, emits
    float time() const { return time_; }
    float duration() const { return duration_; }
    bool finished() const { return time_ >= duration_; }

    Signal<const std::string&> sequenceEvent;

private:
    void applyTime();

    std::vector<SequenceTrackNode*> trackNodes_;  // owned by the Animators
    Timeline propertyTimeline_;
    std::vector<ClipViewEvent> events_;
    float duration_ = 0.0f;
    float time_ = 0.0f;
};

} // namespace saida
