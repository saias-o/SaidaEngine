#include "xr/toolkit/XRController.hpp"
#include "xr/toolkit/XRInput.hpp"

#include <nlohmann/json.hpp>

namespace saida {

namespace {
// Internal: drives the owning XRController node's local transform from XRInput.
// Not serialized (no typeName) — re-added by the XRController constructor on load.
class XRControllerTracker : public Behaviour {
public:
    bool visibleInEditor() const override { return false; }
    void onUpdate(float) override {
        auto* c = static_cast<XRController*>(node());
        const XRHandState& s = XRInput::hand(c->hand());
        // Hold the last good pose when tracking is lost (avoids snapping to origin).
        if (!s.active || !s.grip.tracked) return;
        node()->transform().position = s.grip.position;
        node()->transform().rotation = s.grip.orientation;
    }
};
} // namespace

XRController::XRController(XRHand hand)
    : Node(hand == XRHand::Left ? "XRController (L)" : "XRController (R)"), hand_(hand) {
    addBehaviour<XRControllerTracker>();
}

bool XRController::isTracked() const {
    const XRHandState& s = XRInput::hand(hand_);
    return s.active && s.grip.tracked;
}

void XRController::serialize(nlohmann::json& j, ResourceManager& resources) const {
    Node::serialize(j, resources);
    j["hand"] = static_cast<int>(hand_);
}

void XRController::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    Node::deserialize(j, resources);
    if (j.contains("hand")) hand_ = static_cast<XRHand>(j["hand"].get<int>());
}

} // namespace saida
