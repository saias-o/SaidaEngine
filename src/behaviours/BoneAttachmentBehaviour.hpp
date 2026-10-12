#pragma once

#include "core/Signal.hpp"
#include "scene/Behaviour.hpp"
#include "scene/Transform.hpp"

#include <cstdint>
#include <string>

namespace saida {

class Animator;

// Keeps its node on a bone of the Animator on its parent node: a weapon in a
// hand, a hat on a head, a lantern on a belt. The node's local transform
// becomes the bone's object-space transform composed with `offset` (in the
// bone's own axes), so it inherits the animated node's world transform, scale
// included, like any child.
//
// It is placed when the Animator recomputes its pose (Animator::poseUpdated), the
// same tick, so nothing it holds trails the hand by a frame. A held pose that
// does not change does not move it. Runtime-only: not serialized.
//
// The node must be a direct child of the animated node. Anything else -- no
// Animator on the parent, no rig yet, an unknown bone -- leaves the node where
// it is and is said once in the log (attached() stays false).
class BoneAttachmentBehaviour : public Behaviour {
public:
    BoneAttachmentBehaviour() = default;
    BoneAttachmentBehaviour(std::string bone, Transform offset = {})
        : bone_(std::move(bone)), offset_(offset) {}

    void onReady() override;
    void onUpdate(float dt) override;
    bool visibleInEditor() const override { return false; }

    void setBone(std::string bone);
    const std::string& bone() const { return bone_; }
    void setOffset(const Transform& offset) { offset_ = offset; place(); }
    const Transform& offset() const { return offset_; }

    // Whether the node follows its bone now. A destroyed Animator expires
    // the connection, so this never reads one that is gone.
    bool attached() const { return boneIndex_ >= 0 && animator_ != nullptr && posed_.connected(); }

private:
    void connect();
    void place();

    std::string bone_;
    Transform offset_;
    Animator* animator_ = nullptr;
    int32_t boneIndex_ = -1;
    bool refusalSaid_ = false;
    Connection posed_;
};

} // namespace saida
