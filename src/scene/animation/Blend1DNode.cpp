#include "scene/animation/Blend1DNode.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>

namespace saida {

void Blend1DNode::addInput(float threshold, std::unique_ptr<AnimNode> node) {
    if (!node) return;
    Input input{threshold, std::move(node)};
    const auto position = std::upper_bound(
        inputs_.begin(), inputs_.end(), threshold,
        [](float value, const Input& existing) { return value < existing.threshold; });
    inputs_.insert(position, std::move(input));
}

void Blend1DNode::bindParameter(const AnimBlackboard* blackboard,
                                std::string_view paramName) {
    blackboard_ = blackboard;
    paramHash_ = hashString(paramName);
    hasParam_ = true;
}

void Blend1DNode::activeSpan(size_t& lower, size_t& upper, float& weight) const {
    lower = 0;
    upper = 0;
    weight = 0.0f;
    if (inputs_.size() < 2 || value_ <= inputs_.front().threshold) return;
    if (value_ >= inputs_.back().threshold) {
        lower = upper = inputs_.size() - 1;
        return;
    }
    while (upper + 1 < inputs_.size() && inputs_[upper + 1].threshold <= value_) ++upper;
    lower = upper;
    upper = lower + 1;
    const float span = inputs_[upper].threshold - inputs_[lower].threshold;
    weight = span > 0.0f ? (value_ - inputs_[lower].threshold) / span : 0.0f;
}

void Blend1DNode::update(float deltaTime) {
    if (hasParam_ && blackboard_) value_ = blackboard_->getFloat(paramHash_);
    // All inputs advance: phases stay aligned when the blend
    // switches from one pair to another.
    for (Input& input : inputs_) input.node->update(deltaTime);
}

void Blend1DNode::evaluate(const LocalPose& bindPose, LocalPose& outPose) const {
    if (inputs_.empty()) {
        outPose = bindPose;
        return;
    }

    size_t lower = 0, upper = 0;
    float weight = 0.0f;
    activeSpan(lower, upper, weight);

    if (lower == upper || weight <= 0.0f) {
        inputs_[lower].node->evaluate(bindPose, outPose);
        return;
    }
    if (weight >= 1.0f) {
        inputs_[upper].node->evaluate(bindPose, outPose);
        return;
    }

    inputs_[lower].node->evaluate(bindPose, tempPoseLower_);
    inputs_[upper].node->evaluate(bindPose, tempPoseUpper_);
    outPose.resize(bindPose.localTransforms.size());
    for (size_t i = 0; i < outPose.localTransforms.size(); ++i) {
        const Transform& a = tempPoseLower_.localTransforms[i];
        const Transform& b = tempPoseUpper_.localTransforms[i];
        Transform& out = outPose.localTransforms[i];
        out.position = glm::mix(a.position, b.position, weight);
        out.rotation = glm::slerp(a.rotation, b.rotation, weight);
        out.scale = glm::mix(a.scale, b.scale, weight);
    }
}

float Blend1DNode::normalizedTime() const {
    size_t lower = 0, upper = 0;
    float weight = 0.0f;
    if (inputs_.empty()) return -1.0f;
    activeSpan(lower, upper, weight);
    return inputs_[weight < 0.5f ? lower : upper].node->normalizedTime();
}

void Blend1DNode::seekNormalized(float phase) {
    for (Input& input : inputs_) input.node->seekNormalized(phase);
}

} // namespace saida
