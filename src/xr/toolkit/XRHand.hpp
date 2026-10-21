#pragma once

#include "scene/Node.hpp"
#include "xr/toolkit/XRTypes.hpp"

#include <array>
#include <vector>

namespace saida {

class MeshNode;

// Skinned hand driven by XRInput's world-space OpenXR skeleton. The built-in
// GLB and procedural fallback are runtime children, reconstructed on load.
class XRHandNode : public Node {
public:
    explicit XRHandNode(saida::XRHand hand = saida::XRHand::Left);

    saida::XRHand hand() const { return hand_; }
    void setHand(saida::XRHand hand);
    bool isTracked() const;

    // Called by the internal tracker behaviour once per scene update.
    void updateTrackingVisuals();

    // A hand made in code builds its visuals here (loading does it itself).
    // Calling again rebuilds with a new colour.
    void buildVisuals(ResourceManager& resources);
    void setColor(const glm::vec4& color) { color_ = color; }

    const char* typeName() const override { return "XRHand"; }
    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;

private:
    void setVisualsVisible(bool visible);
    bool buildSkinnedVisuals(ResourceManager& resources);

    saida::XRHand hand_;
    glm::vec4 color_{1.0f};
    float jointScale_ = 1.0f;
    std::array<MeshNode*, kXRHandJointCount> jointNodes_{};
    std::vector<MeshNode*> boneNodes_;
    std::vector<MeshNode*> skinnedNodes_;
    ResourceManager* resources_ = nullptr;
    bool visualsBuilt_ = false;
};

} // namespace saida
