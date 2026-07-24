#pragma once

#include "scene/Node.hpp"
#include "xr/toolkit/XRTypes.hpp"

#include <array>
#include <vector>

namespace saida {

class MeshNode;

// Visible procedural hand driven by XRInput's OpenXR skeleton. The generated
// cube joints/bones are runtime implementation details and are intentionally not
// serialized; a scene only needs one XRHand node per side.
class XRHandNode : public Node {
public:
    explicit XRHandNode(saida::XRHand hand = saida::XRHand::Left);

    saida::XRHand hand() const { return hand_; }
    void setHand(saida::XRHand hand) { hand_ = hand; }
    bool isTracked() const;

    // Called by the internal tracker behaviour once per scene update.
    void updateTrackingVisuals();

    const char* typeName() const override { return "XRHand"; }
    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;

private:
    void buildVisuals(ResourceManager& resources);
    void setVisualsVisible(bool visible);

    saida::XRHand hand_;
    glm::vec4 color_{0.2f, 0.75f, 1.0f, 1.0f};
    float jointScale_ = 1.0f;
    std::array<MeshNode*, kXRHandJointCount> jointNodes_{};
    std::vector<MeshNode*> boneNodes_;
    bool visualsBuilt_ = false;
};

} // namespace saida
