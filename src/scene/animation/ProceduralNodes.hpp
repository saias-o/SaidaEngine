#pragma once

// Procedural nodes applied AFTER animation: two-bone IK (arms/legs) and
// look-at (head). This is the late layer of the XR avatar: targets (hands,
// gaze) are pushed every frame by gameplay or tracking, without
// re-evaluating the upstream animation graph.

#include "scene/animation/AnimNode.hpp"
#include "scene/animation/Rig.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string>

namespace saida {

// Analytic two-segment IK (shoulder->elbow->hand). The chain must be a
// direct parent chain: `mid` a child of `root`, `tip` a child of `mid`.
class TwoBoneIKNode : public AnimNode {
public:
    TwoBoneIKNode(const Rig& rig, const std::string& rootBone,
                  const std::string& midBone, const std::string& tipBone);

    void setInput(std::unique_ptr<AnimNode> node) { input_ = std::move(node); }
    // Target and pole in the character's object space.
    void setTarget(const glm::vec3& target) { target_ = target; }
    void setPole(const glm::vec3& pole);
    void setWeight(float weight) { weight_ = weight; }
    bool valid() const { return rootIndex_ >= 0 && midIndex_ >= 0 && tipIndex_ >= 0; }

    void update(float deltaTime) override;
    void evaluate(const LocalPose& bindPose, LocalPose& outPose) const override;

private:
    const Rig* rig_;
    std::unique_ptr<AnimNode> input_;
    int32_t rootIndex_ = -1;
    int32_t midIndex_ = -1;
    int32_t tipIndex_ = -1;
    glm::vec3 target_{0.0f};
    glm::vec3 pole_{0.0f};
    bool hasPole_ = false;
    float weight_ = 1.0f;

    mutable GlobalPose scratchGlobal_;
};

// Orients a bone's `forward` axis (bone-local space) toward a target.
class LookAtNode : public AnimNode {
public:
    LookAtNode(const Rig& rig, const std::string& bone,
               const glm::vec3& forwardAxis = {0.0f, 0.0f, 1.0f});

    void setInput(std::unique_ptr<AnimNode> node) { input_ = std::move(node); }
    void setTarget(const glm::vec3& target) { target_ = target; }
    void setWeight(float weight) { weight_ = weight; }
    bool valid() const { return boneIndex_ >= 0; }

    void update(float deltaTime) override;
    void evaluate(const LocalPose& bindPose, LocalPose& outPose) const override;

private:
    const Rig* rig_;
    std::unique_ptr<AnimNode> input_;
    int32_t boneIndex_ = -1;
    glm::vec3 forwardAxis_;
    glm::vec3 target_{0.0f};
    float weight_ = 1.0f;

    mutable GlobalPose scratchGlobal_;
};

} // namespace saida
