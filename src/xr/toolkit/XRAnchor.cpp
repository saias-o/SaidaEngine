#include "xr/toolkit/XRAnchor.hpp"
#include "xr/toolkit/XRAnchors.hpp"
#include "xr/toolkit/XRTypes.hpp"

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

namespace saida {

namespace {
// Internal: registers the anchor on ready and tracks the backend pose each frame.
// Not serialized (re-added by the XRAnchor constructor on load).
class XRAnchorTracker : public Behaviour {
public:
    bool visibleInEditor() const override { return false; }
    void onReady() override {
        const glm::mat4& w = node()->worldTransform();
        XRPose pose;
        pose.tracked = true;
        pose.position = glm::vec3(w[3]);
        pose.orientation = glm::normalize(glm::quat_cast(glm::mat3(w)));
        handle_ = XRAnchors::create(pose);
    }
    void onUpdate(float) override {
        if (!handle_) return;  // no backend → keep authored pose
        XRPose pose;
        if (XRAnchors::locate(handle_, pose)) {
            node()->transform().position = pose.position;
            node()->transform().rotation = pose.orientation;
        }
    }
    void onDestroy() override {
        if (handle_) { XRAnchors::destroy(handle_); handle_ = 0; }
    }

private:
    uint64_t handle_ = 0;
};
} // namespace

XRAnchor::XRAnchor() : Node("XRAnchor") {
    addBehaviour<XRAnchorTracker>();
}

void XRAnchor::serialize(nlohmann::json& j, ResourceManager& resources) const {
    Node::serialize(j, resources);
    j["persistent"] = persistent;
}

void XRAnchor::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    Node::deserialize(j, resources);
    if (j.contains("persistent")) persistent = j["persistent"].get<bool>();
}

} // namespace saida
