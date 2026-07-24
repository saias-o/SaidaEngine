#include "scene/animation/LayerNode.hpp"

#include <glm/gtc/quaternion.hpp>

namespace saida {

BoneMask BoneMask::fromChain(const Rig& rig, const std::string& rootBone, float weight) {
    BoneMask mask;
    mask.weights.assign(rig.boneCount(), 0.0f);
    const int32_t root = rig.findBoneIndex(rootBone);
    if (root < 0) return mask;

    mask.weights[size_t(root)] = weight;
    // Parents precede their children in topological order: a single pass
    // is enough to propagate chain membership.
    for (uint32_t boneIndex : rig.evaluationOrder()) {
        const int32_t parent = rig.bones()[boneIndex].parentIndex;
        if (parent >= 0 && mask.weights[size_t(parent)] > 0.0f)
            mask.weights[boneIndex] = weight;
    }
    return mask;
}

void LayerNode::update(float deltaTime) {
    if (base_) base_->update(deltaTime);
    if (overlay_) overlay_->update(deltaTime);
}

void LayerNode::evaluate(const LocalPose& bindPose, LocalPose& outPose) const {
    if (!base_) {
        outPose = bindPose;
    } else {
        base_->evaluate(bindPose, outPose);
    }
    if (!overlay_ || weight_ <= 0.0f) return;

    overlay_->evaluate(bindPose, tempPoseOverlay_);
    for (size_t i = 0; i < outPose.localTransforms.size(); ++i) {
        const float w = weight_ * mask_.weightFor(i);
        if (w <= 0.0f) continue;

        Transform& out = outPose.localTransforms[i];
        const Transform& overlay = tempPoseOverlay_.localTransforms[i];
        if (mode_ == Mode::Override) {
            out.position = glm::mix(out.position, overlay.position, w);
            out.rotation = glm::slerp(out.rotation, overlay.rotation, w);
            out.scale = glm::mix(out.scale, overlay.scale, w);
        } else {
            // Additive: the layer's delta relative to the rest pose, applied
            // on top of the base (rotation composed, translation added).
            const Transform& rest = bindPose.localTransforms[i];
            const glm::quat delta = overlay.rotation * glm::inverse(rest.rotation);
            out.rotation = glm::slerp(glm::quat(1, 0, 0, 0), delta, w) * out.rotation;
            out.position += (overlay.position - rest.position) * w;
            out.scale += (overlay.scale - rest.scale) * w;
        }
    }
}

float LayerNode::normalizedTime() const {
    return base_ ? base_->normalizedTime() : -1.0f;
}

void LayerNode::seekNormalized(float phase) {
    if (base_) base_->seekNormalized(phase);
    if (overlay_) overlay_->seekNormalized(phase);
}

} // namespace saida
