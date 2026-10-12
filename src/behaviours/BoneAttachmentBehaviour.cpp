#include "behaviours/BoneAttachmentBehaviour.hpp"

#include "core/Log.hpp"
#include "scene/Node.hpp"
#include "scene/animation/Animator.hpp"

namespace saida {

void BoneAttachmentBehaviour::onReady() { connect(); }

void BoneAttachmentBehaviour::onUpdate(float) {
    // The Animator may gain its rig (or the node its parent) after this was
    // added: keep asking until it can follow, then the signal does the work.
    if (!attached()) connect();
}

void BoneAttachmentBehaviour::setBone(std::string bone) {
    bone_ = std::move(bone);
    boneIndex_ = -1;
    refusalSaid_ = false;
    connect();
}

void BoneAttachmentBehaviour::connect() {
    Node* self = node();
    Node* parent = self ? self->parent() : nullptr;
    Animator* animator = parent ? parent->getBehaviour<Animator>() : nullptr;
    const Rig* rig = animator ? animator->rig() : nullptr;
    const int32_t index = rig ? rig->findBoneIndex(bone_) : -1;
    if (index < 0) {
        // A rig that is not there yet is a wait, not a refusal; a rig without
        // the bone, or no Animator at all, is said once.
        if (!refusalSaid_ && self && (!animator || rig)) {
            refusalSaid_ = true;
            Log::warn("BoneAttachment '", self->name(), "': ",
                      !animator ? "its parent has no Animator" : "the rig has no bone '" + bone_ + "'",
                      "; the node is not attached");
        }
        return;
    }
    if (animator != animator_ || !posed_.connected()) {
        animator_ = animator;
        posed_ = animator->poseUpdated.connect([this] { place(); });
    }
    boneIndex_ = index;
    place();
}

void BoneAttachmentBehaviour::place() {
    if (!attached() || !node()) return;
    const auto& matrices = animator_->globalPose().globalMatrices;
    if (size_t(boneIndex_) >= matrices.size()) return;
    const glm::mat4 local = matrices[size_t(boneIndex_)] * offset_.matrix();
    Transform& transform = node()->transform();
    transform.position = glm::vec3(local[3]);
    glm::mat3 axes(local);
    glm::vec3 scale(glm::length(axes[0]), glm::length(axes[1]), glm::length(axes[2]));
    for (int c = 0; c < 3; ++c)
        if (scale[c] > 1e-8f) axes[c] /= scale[c];
    transform.rotation = glm::normalize(glm::quat_cast(axes));
    transform.scale = scale;
}

} // namespace saida
