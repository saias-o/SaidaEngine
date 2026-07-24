#pragma once

// LayerNode — overlays a layer (e.g. upper body) onto a base pose,
// restricted by a BoneMask and weighted. Two modes: Override (blends toward
// the layer's pose) and Additive (adds the layer-minus-rest-pose delta).

#include "scene/animation/AnimNode.hpp"
#include "scene/animation/Rig.hpp"

#include <memory>
#include <string>
#include <vector>

namespace saida {

// Per-bone weight [0,1]. An empty mask is 1 everywhere.
struct BoneMask {
    std::vector<float> weights;

    float weightFor(size_t boneIndex) const {
        return boneIndex < weights.size() ? weights[boneIndex] : 1.0f;
    }

    // Mask covering `rootBone` and all its descendants (typical "upper
    // body" chain: fromChain(rig, "Spine")).
    static BoneMask fromChain(const Rig& rig, const std::string& rootBone,
                              float weight = 1.0f);
};

class LayerNode : public AnimNode {
public:
    enum class Mode { Override, Additive };

    void setBase(std::unique_ptr<AnimNode> node) { base_ = std::move(node); }
    void setOverlay(std::unique_ptr<AnimNode> node) { overlay_ = std::move(node); }
    void setMask(BoneMask mask) { mask_ = std::move(mask); }
    void setMode(Mode mode) { mode_ = mode; }
    void setWeight(float weight) { weight_ = weight; }

    void update(float deltaTime) override;
    void evaluate(const LocalPose& bindPose, LocalPose& outPose) const override;
    float normalizedTime() const override;
    void seekNormalized(float phase) override;

private:
    std::unique_ptr<AnimNode> base_;
    std::unique_ptr<AnimNode> overlay_;
    BoneMask mask_;
    Mode mode_ = Mode::Override;
    float weight_ = 1.0f;

    mutable LocalPose tempPoseOverlay_;
};

} // namespace saida
