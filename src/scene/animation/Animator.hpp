#pragma once

#include "core/ReflectionFwd.hpp"
#include "core/Signal.hpp"
#include "scene/Behaviour.hpp"
#include "scene/animation/Rig.hpp"
#include "scene/animation/Pose.hpp"
#include "scene/animation/AnimStateMachine.hpp"
#include "scene/animation/AnimBlackboard.hpp"
#include "scene/animation/Retarget.hpp"
#include "scene/animation/RetargetProfile.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace saida {

class AnimationClip;
class AnimGraphAsset;
class ClipNode;
class ClipView;
struct AssetDiagnostic;

// The Animator drives an animation graph (FSM / blend tree) for a skinned mesh
// and produces the GlobalPose (skinning matrices) the renderer uploads to the GPU.
//
// Two ways to use it:
//   • Simple, by name — `animator->play("Walk")` / `play("Idle")`. The loader
//     registers the model's clips via addClip(), so a character switches
//     idle/walk/jump from a behaviour with automatic crossfades.
//   • Advanced — provide your own AnimStateMachine via setStateMachine() and drive
//     it with blackboard parameters (setFloat/setBool) + conditions.
class Animator : public Behaviour {
public:
    void onUpdate(float dt) override;

    void setRig(Rig* rig);
    const Rig* rig() const { return rig_; }

    // Bind pose fallback used during evaluation (defaults to identity transforms).
    void setBindPose(LocalPose bindPose) { bindPose_ = std::move(bindPose); }

    void addClip(const std::string& name, const AnimationClip* clip) { clips_[name] = clip; }
    const std::unordered_map<std::string, const AnimationClip*>& clips() const { return clips_; }

    // Name-based (see RetargetMap). Affects clips started via play() afterwards.
    void setRetarget(RetargetMap map) { retarget_ = std::move(map); }
    const RetargetMap& retarget() const { return retarget_; }

    // Full profile: name mapping + rest-pose and scale corrections compiled
    // against the rig (applied to the sampled pose).
    void setRetargetProfile(const RetargetProfile& profile);

    void play(const std::string& name, bool loop = true, float crossfade = 0.2f);
    const std::string& currentClip() const { return currentClip_; }

    // Plays a non-destructive view: the source clip is resolved in the
    // library (the name after '#' in view.source), the sub-range/speed/loop
    // apply without duplicating keys. Same internal FSM as play().
    void playView(const ClipView& view, float crossfade = 0.2f);

    // Replaces the graph with a compiled .sgraph: clips resolved in the
    // library, parameter defaults applied to the blackboard. Returns
    // false (with diagnostics) if no state can be built.
    bool setGraph(const AnimGraphAsset& graph,
                  std::vector<AssetDiagnostic>* diagnostics = nullptr);

    void setRootNode(std::unique_ptr<AnimNode> rootNode);
    void setStateMachine(std::unique_ptr<AnimStateMachine> sm);  // wires the blackboard

    AnimBlackboard& blackboard() { return blackboard_; }
    void setFloat(std::string_view p, float v) { blackboard_.setFloat(p, v); }
    void setBool(std::string_view p, bool v) { blackboard_.setBool(p, v); }
    void setTrigger(std::string_view p) { blackboard_.setTrigger(p); }

    // Emitted for each ClipView event crossed by the active clip
    // (including looping and reverse playback), with the event name.
    Signal<const std::string&> animationEvent;

    // Root motion of the active clip (play()/playView() states).
    //   Ignore      — root translation stays in the pose (default).
    //   Extract     — accumulated delta to consume via consumeRootMotion().
    //   ApplyToNode — the delta is applied to the owning node on every update.
    enum class RootMotionMode { Ignore, Extract, ApplyToNode };
    void setRootMotion(RootMotionMode mode) { rootMotionMode_ = mode; }
    RootMotionMode rootMotion() const { return rootMotionMode_; }
    glm::vec3 consumeRootMotion();

    // Animation LOD: the graph (states, transitions, events) advances on
    // every tick, but the pose is only resampled at `hz` and interpolated
    // between the two latest samples. 0 = pose every tick.
    void setPoseRate(float hz);
    float poseRate() const { return poseRate_; }

    const GlobalPose& globalPose() const { return globalPose_; }
    AnimNode* rootNode() const { return rootNode_.get(); }

    // The ClipNode currently driving the pose: the root itself, or the active
    // state of the play()-by-name FSM. Null when a custom graph is in control.
    // Editor preview hook (play/pause/scrub of the selected clip).
    ClipNode* activeClipNode() const;

    const char* typeName() const override { return "Animator"; }

    // Reflection: signals only (animationEvent) — serialization stays the
    // hand-written one (SAIDA_REFLECT_BEHAVIOUR would change the on-disk
    // format). Lets scripts do `node.on("animationEvent", fn)`.
    static constexpr const char* reflectName() { return "Animator"; }
    static void describe(reflect::TypeBuilder<Animator>& t);

private:
    void refreshRootMotionExtraction();
    void dispatchClipEvents();
    void samplePose(float dt);

    Rig* rig_ = nullptr;
    std::unique_ptr<AnimNode> rootNode_;

    // Clip library + the lazily-built FSM that backs play()-by-name.
    std::unordered_map<std::string, const AnimationClip*> clips_;
    AnimStateMachine* playbackFsm_ = nullptr;  // non-owning; == rootNode_ when in use
    std::unordered_set<std::string> playStates_;
    std::string currentClip_;

    AnimBlackboard blackboard_;
    RetargetMap retarget_;
    RetargetCorrections retargetCorrections_;
    LocalPose bindPose_;
    LocalPose currentLocalPose_;
    GlobalPose globalPose_;

    RootMotionMode rootMotionMode_ = RootMotionMode::Ignore;
    glm::vec3 pendingRootMotion_{0.0f};

    float poseRate_ = 0.0f;
    float poseAccumulator_ = 0.0f;
    bool sampledPosesPrimed_ = false;
    LocalPose previousSampledPose_;
    LocalPose lastSampledPose_;
};

} // namespace saida
