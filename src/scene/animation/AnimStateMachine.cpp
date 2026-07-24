#include "scene/animation/AnimStateMachine.hpp"
#include <algorithm>
#include <glm/gtc/quaternion.hpp>

namespace saida {

void AnimStateMachine::addState(std::unique_ptr<AnimState> state) {
    if (!state) return;
    std::string name = state->name();
    if (states_.empty()) {
        currentState_ = state.get();
    }
    states_[name] = std::move(state);
}

void AnimStateMachine::transitionTo(const std::string& stateName, float crossfadeDuration) {
    auto it = states_.find(stateName);
    if (it != states_.end()) {
        if (currentState_ == it->second.get()) return;

        crossfadeDuration_ = std::max(0.0f, crossfadeDuration);
        crossfadeTime_ = 0.0f;
        // Without a crossfade, the switch is instant: no previous state to
        // evaluate, and transitions stay immediately re-evaluable.
        previousState_ = crossfadeDuration_ > 0.0f ? currentState_ : nullptr;
        currentState_ = it->second.get();
    }
}

void AnimStateMachine::update(float deltaTime) {
    if (crossfadeDuration_ > 0.0f && previousState_) {
        crossfadeTime_ += deltaTime;
        if (crossfadeTime_ >= crossfadeDuration_) {
            // Transition complete
            previousState_ = nullptr;
            crossfadeDuration_ = 0.0f;
        } else {
            previousState_->update(deltaTime);
        }
    }
    
    if (currentState_) {
        // Evaluate automatic transitions (never during a crossfade).
        if (previousState_ == nullptr) {
            const float phase = currentState_->normalizedTime();
            for (const auto& trans : currentState_->transitions()) {
                if (trans.exitTime >= 0.0f && (phase < 0.0f || phase < trans.exitTime))
                    continue;
                if (!trans.evaluate(blackboard_)) continue;

                if (blackboard_) {
                    for (const auto& cond : trans.conditions)
                        if (cond.isTrigger) blackboard_->resetTrigger(cond.paramHash);
                }
                transitionTo(trans.targetState, trans.crossfadeDuration);
                if (trans.syncPhase && currentState_ && currentState_->node() && phase >= 0.0f)
                    currentState_->node()->seekNormalized(phase);
                break;  // Execute only the first valid transition
            }
        }

        currentState_->update(deltaTime);
    }
}

void AnimStateMachine::evaluate(const LocalPose& bindPose, LocalPose& outPose) const {
    if (!currentState_) {
        // Fallback
        outPose.resize(bindPose.localTransforms.size());
        for (size_t i = 0; i < outPose.localTransforms.size(); ++i) {
            outPose.localTransforms[i] = bindPose.localTransforms[i];
        }
        return;
    }

    if (crossfadeDuration_ > 0.0f && previousState_) {
        float w = std::clamp(crossfadeTime_ / crossfadeDuration_, 0.0f, 1.0f);

        previousState_->evaluate(bindPose, tempPosePrevious_);
        currentState_->evaluate(bindPose, tempPoseCurrent_);

        outPose.resize(bindPose.localTransforms.size());

        for (size_t i = 0; i < outPose.localTransforms.size(); ++i) {
            auto& outTrans = outPose.localTransforms[i];

            if (i < tempPosePrevious_.localTransforms.size() && i < tempPoseCurrent_.localTransforms.size()) {
                const auto& transPrev = tempPosePrevious_.localTransforms[i];
                const auto& transCurr = tempPoseCurrent_.localTransforms[i];

                outTrans.position = glm::mix(transPrev.position, transCurr.position, w);
                outTrans.rotation = glm::slerp(transPrev.rotation, transCurr.rotation, w);
                outTrans.scale    = glm::mix(transPrev.scale, transCurr.scale, w);
            } else if (i < tempPosePrevious_.localTransforms.size()) {
                outTrans = tempPosePrevious_.localTransforms[i];
            } else if (i < tempPoseCurrent_.localTransforms.size()) {
                outTrans = tempPoseCurrent_.localTransforms[i];
            } else {
                outTrans = bindPose.localTransforms[i];
            }
        }
    } else {
        // No transition active
        currentState_->evaluate(bindPose, outPose);
    }
}

} // namespace saida
