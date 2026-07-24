#pragma once

#include "scene/Node.hpp"
#include "xr/toolkit/XRTypes.hpp"

namespace saida {

// Tracked hand/controller node. The internal tracker writes the grip pose locally,
// so parenting under XROrigin composes locomotion normally.
class XRController : public Node {
public:
    explicit XRController(XRHand hand = XRHand::Right);

    XRHand hand() const { return hand_; }
    void setHand(XRHand h) { hand_ = h; }

    // True while the underlying device reported tracking this frame.
    bool isTracked() const;

    const char* typeName() const override { return "XRController"; }
    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;

private:
    XRHand hand_;
};

} // namespace saida
