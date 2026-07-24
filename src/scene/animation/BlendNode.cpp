#include "scene/animation/BlendNode.hpp"
#include <algorithm>
#include <glm/gtc/quaternion.hpp>

namespace saida {

void BlendNode::update(float deltaTime) {
    if (inputA_) inputA_->update(deltaTime);
    if (inputB_) inputB_->update(deltaTime);
}

void BlendNode::evaluate(const LocalPose& bindPose, LocalPose& outPose) const {
    if (!inputA_ && !inputB_) {
        outPose.resize(bindPose.localTransforms.size());
        for (size_t i = 0; i < outPose.localTransforms.size(); ++i) {
            outPose.localTransforms[i] = bindPose.localTransforms[i];
        }
        return;
    }
    
    if (!inputA_) {
        inputB_->evaluate(bindPose, outPose);
        return;
    }
    
    if (!inputB_) {
        inputA_->evaluate(bindPose, outPose);
        return;
    }

    float w = std::clamp(weight_, 0.0f, 1.0f);

    inputA_->evaluate(bindPose, tempPoseA_);
    inputB_->evaluate(bindPose, tempPoseB_);

    outPose.resize(bindPose.localTransforms.size());

    for (size_t i = 0; i < outPose.localTransforms.size(); ++i) {
        auto& outTrans = outPose.localTransforms[i];

        if (i < tempPoseA_.localTransforms.size() && i < tempPoseB_.localTransforms.size()) {
            const auto& transA = tempPoseA_.localTransforms[i];
            const auto& transB = tempPoseB_.localTransforms[i];

            outTrans.position = glm::mix(transA.position, transB.position, w);
            outTrans.rotation = glm::slerp(transA.rotation, transB.rotation, w);
            outTrans.scale    = glm::mix(transA.scale, transB.scale, w);
        } else if (i < tempPoseA_.localTransforms.size()) {
            outTrans = tempPoseA_.localTransforms[i];
        } else if (i < tempPoseB_.localTransforms.size()) {
            outTrans = tempPoseB_.localTransforms[i];
        } else {
            outTrans = bindPose.localTransforms[i];
        }
    }
}

} // namespace saida
