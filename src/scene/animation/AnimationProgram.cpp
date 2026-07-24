#include "scene/animation/AnimationProgram.hpp"

#include <algorithm>
#include <cmath>

namespace saida {

namespace {

void addDiagnostic(std::vector<AssetDiagnostic>* diagnostics, const char* code,
                   const std::string& jsonPath, const std::string& message) {
    if (diagnostics)
        diagnostics->push_back({code, AssetDiagnostic::Severity::Error, jsonPath, message});
}

bool evaluateCondition(const ProgramCondition& condition, const float* params) {
    const float value = params[condition.paramIndex];
    switch (condition.op) {
        case ConditionOp::Equals: return value == condition.value;
        case ConditionOp::NotEquals: return value != condition.value;
        case ConditionOp::Greater: return value > condition.value;
        case ConditionOp::Less: return value < condition.value;
        case ConditionOp::GreaterEquals: return value >= condition.value;
        case ConditionOp::LessEquals: return value <= condition.value;
    }
    return false;
}

ConditionOp conditionOpFromString(const std::string& op) {
    if (op == "==") return ConditionOp::Equals;
    if (op == "!=") return ConditionOp::NotEquals;
    if (op == ">") return ConditionOp::Greater;
    if (op == "<") return ConditionOp::Less;
    if (op == ">=") return ConditionOp::GreaterEquals;
    return ConditionOp::LessEquals;
}

} // namespace

std::shared_ptr<const AnimationProgram> AnimationProgram::compile(
    const AnimGraphAsset& graph, const ClipResolver& resolveClip,
    std::vector<AssetDiagnostic>* diagnostics) {
    auto program = std::make_shared<AnimationProgram>();

    for (const AnimGraphParam& param : graph.parameters) {
        program->paramNames_.push_back(param.name);
        program->paramDefaults_.push_back(param.defaultValue);
    }

    // Cooked clips shared between states, resolved once per key.
    std::vector<std::string> clipKeys;
    const auto clipIndexFor = [&](const std::string& key) -> int {
        for (size_t i = 0; i < clipKeys.size(); ++i)
            if (clipKeys[i] == key) return int(i);
        std::shared_ptr<const CookedClip> clip = resolveClip(key);
        if (!clip) return -1;
        clipKeys.push_back(key);
        program->clips_.push_back(std::move(clip));
        return int(program->clips_.size() - 1);
    };

    for (const AnimGraphState& state : graph.states) {
        const AnimGraphClipRef* ref = graph.findClip(state.play);
        const int clipIndex = ref ? clipIndexFor(ref->key) : -1;
        if (clipIndex < 0) {
            addDiagnostic(diagnostics, "animprogram.build.clip_unresolved",
                          "/states/" + state.name,
                          "no cooked clip for alias '" + state.play + "'");
            continue;
        }
        ProgramState compiled;
        compiled.clipIndex = uint32_t(clipIndex);
        compiled.loop = state.loop;
        compiled.speed = state.speed;
        compiled.cursorOffset = program->totalCursorCount_;
        program->totalCursorCount_ +=
            uint32_t(program->clips_[size_t(clipIndex)]->trackCount());
        program->states_.push_back(compiled);
        program->stateNames_.push_back(state.name);
    }
    if (program->states_.empty()) {
        addDiagnostic(diagnostics, "animprogram.build.no_states", "/states",
                      "no state could be compiled");
        return nullptr;
    }

    const auto stateIndexFor = [&](const std::string& name) -> int {
        for (size_t i = 0; i < program->stateNames_.size(); ++i)
            if (program->stateNames_[i] == name) return int(i);
        return -1;
    };

    // Transitions sorted by source state for a contiguous runtime walk.
    struct PendingTransition {
        ProgramTransition transition;
        std::vector<ProgramCondition> conditions;
    };
    std::vector<PendingTransition> pending;
    for (const AnimGraphTransition& transition : graph.transitions) {
        const int from = stateIndexFor(transition.from);
        const int to = stateIndexFor(transition.to);
        if (from < 0 || to < 0) {
            addDiagnostic(diagnostics, "animprogram.build.transition_unresolved",
                          "/transitions",
                          "'" + transition.from + "' -> '" + transition.to +
                              "' references a missing state");
            continue;
        }
        PendingTransition entry;
        entry.transition.fromState = uint32_t(from);
        entry.transition.toState = uint32_t(to);
        entry.transition.crossfade = transition.crossfade;
        entry.transition.exitTime = transition.exitTime;
        entry.transition.syncPhase = transition.syncPhase;
        bool conditionsOk = true;
        for (const AnimGraphCondition& condition : transition.when) {
            const int paramIndex = program->paramIndex(condition.param);
            if (paramIndex < 0) {
                addDiagnostic(diagnostics, "animprogram.build.param_unknown",
                              "/transitions", "unknown parameter '" + condition.param + "'");
                conditionsOk = false;
                break;
            }
            const AnimGraphParam* param = graph.findParam(condition.param);
            entry.conditions.push_back(
                {uint32_t(paramIndex), conditionOpFromString(condition.op), condition.value,
                 param && param->type == AnimParamType::Trigger});
        }
        if (conditionsOk) pending.push_back(std::move(entry));
    }
    std::stable_sort(pending.begin(), pending.end(),
                     [](const PendingTransition& a, const PendingTransition& b) {
                         return a.transition.fromState < b.transition.fromState;
                     });
    for (PendingTransition& entry : pending) {
        entry.transition.firstCondition = uint32_t(program->conditions_.size());
        entry.transition.conditionCount = uint32_t(entry.conditions.size());
        program->conditions_.insert(program->conditions_.end(), entry.conditions.begin(),
                                    entry.conditions.end());
        ProgramState& from = program->states_[entry.transition.fromState];
        if (from.transitionCount == 0)
            from.firstTransition = uint32_t(program->transitions_.size());
        ++from.transitionCount;
        program->transitions_.push_back(entry.transition);
    }

    const int initial = stateIndexFor(graph.initial);
    program->initialState_ = initial >= 0 ? uint32_t(initial) : 0u;
    return program;
}

std::shared_ptr<const AnimationProgram> AnimationProgram::singleClip(
    std::shared_ptr<const CookedClip> clip, bool loop) {
    auto program = std::make_shared<AnimationProgram>();
    program->stateNames_.push_back(clip->name());
    ProgramState state;
    state.loop = loop;
    program->totalCursorCount_ = uint32_t(clip->trackCount());
    program->clips_.push_back(std::move(clip));
    program->states_.push_back(state);
    return program;
}

int AnimationProgram::paramIndex(const std::string& name) const {
    for (size_t i = 0; i < paramNames_.size(); ++i)
        if (paramNames_[i] == name) return int(i);
    return -1;
}

AnimationInstance::AnimationInstance(std::shared_ptr<const AnimationProgram> program,
                                     const Rig& rig)
    : program_(std::move(program)), rig_(&rig) {
    params_ = program_->paramDefaults();
    cursors_.resize(program_->totalCursorCount());

    restPose_.resize(rig.boneCount());
    for (size_t i = 0; i < rig.boneCount(); ++i)
        restPose_.localTransforms[i] = rig.bones()[i].restLocal;
    blendedPose_ = restPose_;
    previousPose_ = restPose_;
    globalPose_.resize(rig.boneCount());

    currentState_ = program_->initialState();
    previousState_ = currentState_;
}

const std::string& AnimationInstance::currentStateName() const {
    return program_->stateName(currentState_);
}

void AnimationInstance::enterState(uint32_t state, float crossfade) {
    previousState_ = currentState_;
    previousStateTime_ = stateTime_;
    currentState_ = state;
    stateTime_ = 0.0f;
    crossfading_ = crossfade > 0.0f;
    crossfadeTime_ = 0.0f;
    crossfadeDuration_ = crossfade;
}

float AnimationInstance::advanceTime(const ProgramState& state, float time,
                                     float deltaTime) const {
    const float duration = program_->clip(state.clipIndex).duration();
    if (duration <= 0.0f) return 0.0f;
    float advanced = time + deltaTime * state.speed;
    if (!state.loop) return std::clamp(advanced, 0.0f, duration);
    advanced = std::fmod(advanced, duration);
    return advanced < 0.0f ? advanced + duration : advanced;
}

void AnimationInstance::samplePose(const ProgramState& state, float time, LocalPose& pose) {
    pose = restPose_;  // same size: copy without reallocating
    const CookedClip& clip = program_->clip(state.clipIndex);
    if (clip.trackCount() == 0) return;
    clip.sample(time, pose, cursors_.data() + state.cursorOffset);
}

void AnimationInstance::update(float deltaTime) {
    // Control: transitions of the current state, never during a crossfade
    // (same rules as the authoring state machine).
    const ProgramState* state = &program_->states()[currentState_];
    if (!crossfading_) {
        const float clipDuration = program_->clip(state->clipIndex).duration();
        const float phase = clipDuration > 0.0f ? stateTime_ / clipDuration : 0.0f;
        for (uint32_t t = 0; t < state->transitionCount; ++t) {
            const ProgramTransition& transition =
                program_->transitions()[state->firstTransition + t];
            if (transition.exitTime >= 0.0f && phase < transition.exitTime) continue;
            bool taken = transition.conditionCount > 0 || transition.exitTime >= 0.0f;
            for (uint32_t c = 0; c < transition.conditionCount && taken; ++c) {
                taken = evaluateCondition(
                    program_->conditions()[transition.firstCondition + c], params_.data());
            }
            if (taken) {
                for (uint32_t c = 0; c < transition.conditionCount; ++c) {
                    const ProgramCondition& condition =
                        program_->conditions()[transition.firstCondition + c];
                    if (condition.isTrigger) params_[condition.paramIndex] = 0.0f;
                }
                enterState(transition.toState, transition.crossfade);
                state = &program_->states()[currentState_];
                if (transition.syncPhase) {
                    const float duration = program_->clip(state->clipIndex).duration();
                    stateTime_ = phase * duration;
                }
                break;
            }
        }
    }

    stateTime_ = advanceTime(*state, stateTime_, deltaTime);
    if (crossfading_) {
        const ProgramState& previous = program_->states()[previousState_];
        previousStateTime_ = advanceTime(previous, previousStateTime_, deltaTime);
        crossfadeTime_ += deltaTime;
        if (crossfadeTime_ >= crossfadeDuration_) crossfading_ = false;
    }

    // Sampling, then solve.
    samplePose(*state, stateTime_, blendedPose_);
    if (crossfading_) {
        samplePose(program_->states()[previousState_], previousStateTime_, previousPose_);
        const float weight = crossfadeTime_ / crossfadeDuration_;
        for (size_t i = 0; i < blendedPose_.localTransforms.size(); ++i) {
            Transform& out = blendedPose_.localTransforms[i];
            const Transform& from = previousPose_.localTransforms[i];
            out.position = glm::mix(from.position, out.position, weight);
            out.scale = glm::mix(from.scale, out.scale, weight);
            out.rotation = glm::slerp(from.rotation, out.rotation, weight);
        }
    }
    globalPose_.computeFrom(blendedPose_, *rig_);
}

} // namespace saida
