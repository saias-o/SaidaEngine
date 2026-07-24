#pragma once

// Blend1DNode — 1D blend space: each input is placed at a threshold
// (e.g. speed 0 = Idle, 1.5 = Walk, 4 = Run) and a Blackboard parameter
// picks the neighboring pair to blend. This is the standard locomotion
// recipe at the simple authoring level: no visual graph required.

#include "scene/animation/AnimBlackboard.hpp"
#include "scene/animation/AnimNode.hpp"

#include <memory>
#include <vector>

namespace saida {

class Blend1DNode : public AnimNode {
public:
    // Inputs are kept sorted by threshold.
    void addInput(float threshold, std::unique_ptr<AnimNode> node);

    // The parameter is read on every update; setValue() drives it without a Blackboard.
    void bindParameter(const AnimBlackboard* blackboard, std::string_view paramName);
    void setValue(float value) { value_ = value; }
    float value() const { return value_; }

    void update(float deltaTime) override;
    void evaluate(const LocalPose& bindPose, LocalPose& outPose) const override;
    float normalizedTime() const override;
    void seekNormalized(float phase) override;

private:
    struct Input {
        float threshold = 0.0f;
        std::unique_ptr<AnimNode> node;
    };

    // Pair bracketing the current value and weight of the second input.
    void activeSpan(size_t& lower, size_t& upper, float& weight) const;

    std::vector<Input> inputs_;
    const AnimBlackboard* blackboard_ = nullptr;
    uint32_t paramHash_ = 0;
    bool hasParam_ = false;
    float value_ = 0.0f;

    mutable LocalPose tempPoseLower_;
    mutable LocalPose tempPoseUpper_;
};

} // namespace saida
