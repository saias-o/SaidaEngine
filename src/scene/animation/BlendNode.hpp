#pragma once

#include "scene/animation/AnimNode.hpp"
#include <memory>
#include <vector>

namespace saida {

// BlendNode blends two input poses based on a weight factor.
class BlendNode : public AnimNode {
public:
    BlendNode() = default;

    void update(float deltaTime) override;
    void evaluate(const LocalPose& bindPose, LocalPose& outPose) const override;

    void setInputA(std::unique_ptr<AnimNode> node) { inputA_ = std::move(node); }
    void setInputB(std::unique_ptr<AnimNode> node) { inputB_ = std::move(node); }
    
    // Weight: 0.0 -> 100% InputA, 1.0 -> 100% InputB
    void setWeight(float weight) { weight_ = weight; }

private:
    std::unique_ptr<AnimNode> inputA_;
    std::unique_ptr<AnimNode> inputB_;
    float weight_ = 0.5f;

    // Temporary pose buffers to hold the evaluated results of A and B
    mutable LocalPose tempPoseA_;
    mutable LocalPose tempPoseB_;
};

} // namespace saida
