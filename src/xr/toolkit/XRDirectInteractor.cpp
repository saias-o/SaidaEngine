#include "xr/toolkit/XRDirectInteractor.hpp"
#include "xr/toolkit/XRController.hpp"
#include "xr/toolkit/XRGrabbable.hpp"
#include "xr/toolkit/XRTouchable.hpp"
#include "xr/toolkit/XRInput.hpp"
#include "xr/toolkit/XRTypes.hpp"

#include "scene/Node.hpp"
#include "scene/SceneTree.hpp"
#include "graphics/Mesh.hpp"
#include "core/Log.hpp"

#include <algorithm>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

namespace saida {

namespace {
// World-space bounding radius of a node's mesh (0 if it has none), so the hand
// can grab/poke near the surface rather than only the node origin.
float boundingRadius(Node* n) {
    Mesh* m = n->mesh();
    if (!m) return 0.0f;
    const glm::vec3 e = m->bounds().extent();
    const glm::mat4& w = n->worldTransform();
    const float maxScale = std::max({glm::length(glm::vec3(w[0])),
                                     glm::length(glm::vec3(w[1])),
                                     glm::length(glm::vec3(w[2]))});
    return 0.5f * glm::length(e) * maxScale;
}

glm::vec3 worldPos(Node* n) { return glm::vec3(n->worldTransform()[3]); }
} // namespace

void XRDirectInteractor::onReady() {
    controller_ = dynamic_cast<XRController*>(node());
    if (!controller_)
        Log::warn("XRDirectInteractor must be attached to an XRController node");
}

void XRDirectInteractor::onUpdate(float) {
    if (!controller_) return;
    SceneTree* t = tree();
    if (!t) return;

    const XRHand hand = controller_->hand();
    const glm::vec3 handPos = worldPos(controller_);

    // pointer → no dangling if it was freed).
    XRGrabbable* held = nullptr;
    for (Node* n : t->group(kXRGrabbableGroup))
        if (XRGrabbable* g = n->getBehaviour<XRGrabbable>(); g && g->holder() == controller_) {
            held = g;
            break;
        }

    if (held) {
        if (XRInput::squeezeReleased(hand)) held->release();
    } else if (XRInput::squeezePressed(hand) && XRInput::hand(hand).active) {
        XRGrabbable* best = nullptr;
        float bestDist = 0.0f;
        for (Node* n : t->group(kXRGrabbableGroup)) {
            XRGrabbable* g = n->getBehaviour<XRGrabbable>();
            if (!g || g->isGrabbed()) continue;
            const float d = glm::distance(handPos, worldPos(n));
            if (d <= g->grabRadius + reach + boundingRadius(n) && (!best || d < bestDist)) {
                best = g;
                bestDist = d;
            }
        }
        if (best) best->grab(controller_);
    }

    for (Node* n : t->group(kXRTouchableGroup)) {
        XRTouchable* tb = n->getBehaviour<XRTouchable>();
        if (!tb) continue;
        const bool inRange = glm::distance(handPos, worldPos(n)) <=
                             tb->touchRadius + reach + boundingRadius(n);
        tb->updateTouch(controller_, inRange);
    }
}

void XRDirectInteractor::onDestroy() {
    SceneTree* t = tree();
    if (!t || !controller_) return;
    for (Node* n : t->group(kXRGrabbableGroup))
        if (XRGrabbable* g = n->getBehaviour<XRGrabbable>(); g && g->holder() == controller_)
            g->release();
    for (Node* n : t->group(kXRTouchableGroup))
        if (XRTouchable* tb = n->getBehaviour<XRTouchable>())
            tb->updateTouch(controller_, false);
}

void XRDirectInteractor::save(nlohmann::json& j) const { j["reach"] = reach; }

void XRDirectInteractor::load(const nlohmann::json& j) {
    if (j.contains("reach")) reach = j["reach"].get<float>();
}

} // namespace saida
