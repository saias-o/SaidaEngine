#pragma once

// AnimationProgram / AnimationInstance — the data-oriented runtime core.
//
// The program is the compiled, immutable, shareable form of a playback
// graph: states bound to cooked clips, transitions with conditions typed
// by parameter index, precomputed cursor offsets. The instance carries all
// per-character state (parameters, time, cursors, poses) reserved at
// construction: `update()` allocates nothing, compares no strings, and
// makes no virtual calls.

#include "scene/animation/AnimGraphAsset.hpp"
#include "scene/animation/AnimStateMachine.hpp"  // ConditionOp
#include "scene/animation/CookedClip.hpp"
#include "scene/animation/Rig.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace saida {

struct ProgramState {
    uint32_t clipIndex = 0;
    bool loop = true;
    float speed = 1.0f;
    uint32_t cursorOffset = 0;      // start of this state's cursors in the instance
    uint32_t firstTransition = 0;   // transitions sorted by source state
    uint32_t transitionCount = 0;
};

struct ProgramCondition {
    uint32_t paramIndex = 0;
    ConditionOp op = ConditionOp::Greater;
    float value = 0.0f;
    bool isTrigger = false;  // reset to 0 once the transition is taken
};

struct ProgramTransition {
    uint32_t fromState = 0;
    uint32_t toState = 0;
    float crossfade = 0.0f;
    float exitTime = -1.0f;  // minimum normalized exit phase (< 0 = free)
    bool syncPhase = false;  // the target state starts at the source state's phase
    uint32_t firstCondition = 0;
    uint32_t conditionCount = 0;
};

class AnimationProgram {
public:
    using ClipResolver =
        std::function<std::shared_ptr<const CookedClip>(const std::string& key)>;

    // Compiles a .sgraph: clips resolved into cooked clips, flattened
    // parameters, conditions translated into indices. Null if no state can be built.
    static std::shared_ptr<const AnimationProgram> compile(
        const AnimGraphAsset& graph, const ClipResolver& resolveClip,
        std::vector<AssetDiagnostic>* diagnostics = nullptr);

    // Trivial single looping-state program — the compiled `Animator::play` path.
    static std::shared_ptr<const AnimationProgram> singleClip(
        std::shared_ptr<const CookedClip> clip, bool loop = true);

    const std::vector<ProgramState>& states() const { return states_; }
    const std::vector<ProgramTransition>& transitions() const { return transitions_; }
    const std::vector<ProgramCondition>& conditions() const { return conditions_; }
    uint32_t initialState() const { return initialState_; }
    uint32_t totalCursorCount() const { return totalCursorCount_; }

    const CookedClip& clip(uint32_t index) const { return *clips_[index]; }
    const std::string& stateName(uint32_t index) const { return stateNames_[index]; }

    // Authoring-name-to-index binding. Only call this outside the hot loop.
    int paramIndex(const std::string& name) const;
    size_t paramCount() const { return paramNames_.size(); }
    const std::vector<float>& paramDefaults() const { return paramDefaults_; }

private:
    std::vector<std::shared_ptr<const CookedClip>> clips_;
    std::vector<ProgramState> states_;
    std::vector<std::string> stateNames_;
    std::vector<ProgramTransition> transitions_;
    std::vector<ProgramCondition> conditions_;
    std::vector<std::string> paramNames_;
    std::vector<float> paramDefaults_;
    uint32_t initialState_ = 0;
    uint32_t totalCursorCount_ = 0;
};

class AnimationInstance {
public:
    // All of the instance's memory is reserved here.
    AnimationInstance(std::shared_ptr<const AnimationProgram> program, const Rig& rig);

    // Control (transitions), sampling (cursors), then solve (blend + global).
    void update(float deltaTime);

    void setParam(uint32_t index, float value) { params_[index] = value; }
    float param(uint32_t index) const { return params_[index]; }

    uint32_t currentState() const { return currentState_; }
    const std::string& currentStateName() const;
    float stateTime() const { return stateTime_; }
    void seek(float stateTime) { stateTime_ = stateTime; }

    const LocalPose& localPose() const { return blendedPose_; }
    const GlobalPose& globalPose() const { return globalPose_; }
    const AnimationProgram& program() const { return *program_; }

private:
    void enterState(uint32_t state, float crossfade);
    float advanceTime(const ProgramState& state, float time, float deltaTime) const;
    void samplePose(const ProgramState& state, float time, LocalPose& pose);

    std::shared_ptr<const AnimationProgram> program_;
    const Rig* rig_ = nullptr;

    std::vector<float> params_;
    std::vector<CookedCursor> cursors_;

    uint32_t currentState_ = 0;
    uint32_t previousState_ = 0;
    float stateTime_ = 0.0f;
    float previousStateTime_ = 0.0f;
    float crossfadeTime_ = 0.0f;
    float crossfadeDuration_ = 0.0f;
    bool crossfading_ = false;

    LocalPose restPose_;
    LocalPose blendedPose_;
    LocalPose previousPose_;
    GlobalPose globalPose_;
};

} // namespace saida
