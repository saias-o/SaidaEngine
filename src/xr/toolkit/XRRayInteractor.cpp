#include "xr/toolkit/XRRayInteractor.hpp"
#include "xr/toolkit/XRController.hpp"
#include "xr/toolkit/XROrigin.hpp"
#include "xr/toolkit/TeleportArea.hpp"
#include "xr/toolkit/XRInput.hpp"
#include "xr/toolkit/XRTypes.hpp"

#include "scene/Node.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneTree.hpp"
#include "nodes/WebCanvasNode.hpp"
#include "core/Log.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>

namespace saida {

void XRRayInteractor::onReady() {
    controller_ = dynamic_cast<XRController*>(node());
    if (!controller_)
        Log::warn("XRRayInteractor must be attached to an XRController node");
}

void XRRayInteractor::onUpdate(float) {
    if (!controller_) return;
    SceneTree* t = tree();
    if (!t) return;

    const XRHand hand = controller_->hand();
    const XRHandState& s = XRInput::hand(hand);

    // Ray from the aim pose (fallback to grip); forward is -Z in pose space.
    const XRPose& aim = s.aim.tracked ? s.aim : s.grip;
    rayOrigin_ = aim.position;
    rayDir_ = glm::normalize(aim.orientation * glm::vec3(0.0f, 0.0f, -1.0f));

    WebCanvasNode* uiTarget = nullptr;
    glm::vec2 uiLocal{0.0f};
    float uiDistance = std::numeric_limits<float>::max();
    if (t->mounted()) {
        for (WebCanvasNode* canvas : t->world().webCanvases()) {
            if (!canvas || !canvas->isActiveInHierarchy() || !canvas->interactive()) continue;
            if (canvas->mode() != WebCanvasNode::Mode::WorldSpace) continue;
            glm::vec2 local{0.0f};
            float distance = 0.0f;
            if (canvas->raycast(rayOrigin_, rayDir_, local, distance) && distance <= maxDistance && distance < uiDistance) {
                uiTarget = canvas;
                uiLocal = {
                    std::clamp(local.x, 0.0f, static_cast<float>(canvas->width())),
                    std::clamp(local.y, 0.0f, static_cast<float>(canvas->height()))
                };
                uiDistance = distance;
            }
        }
    }

    if (uiTarget) {
        uiTarget->fireMouseEvent(WebCanvasNode::MouseEvent::Move,
            static_cast<int>(uiLocal.x), static_cast<int>(uiLocal.y),
            XRInput::triggerDown(hand) ? WebCanvasNode::MouseButton::Left : WebCanvasNode::MouseButton::None);
    }
    if (XRInput::triggerPressed(hand) && uiTarget) {
        uiPressedTarget_ = uiTarget;
        uiPressedLocal_ = uiLocal;
        uiTarget->fireMouseEvent(WebCanvasNode::MouseEvent::Down,
            static_cast<int>(uiLocal.x), static_cast<int>(uiLocal.y), WebCanvasNode::MouseButton::Left);
    }
    if (XRInput::triggerReleased(hand) && uiPressedTarget_) {
        WebCanvasNode* releaseTarget = uiPressedTarget_;
        glm::vec2 releaseLocal = releaseTarget == uiTarget ? uiLocal : uiPressedLocal_;
        if (releaseTarget->isActiveInHierarchy()) {
            releaseLocal = {
                std::clamp(releaseLocal.x, 0.0f, static_cast<float>(releaseTarget->width())),
                std::clamp(releaseLocal.y, 0.0f, static_cast<float>(releaseTarget->height()))
            };
            releaseTarget->fireMouseEvent(WebCanvasNode::MouseEvent::Up,
                static_cast<int>(releaseLocal.x), static_cast<int>(releaseLocal.y), WebCanvasNode::MouseButton::Left);
        }
        uiPressedTarget_ = nullptr;
    }

    const bool wantAim = s.active && s.thumbstick.y > aimThreshold;

    if (wantAim) {
        aiming_ = true;
        hasTarget_ = false;
        targetArea_ = nullptr;
        float bestDist = maxDistance;
        for (Node* n : t->group(kXRTeleportAreaGroup)) {
            auto* area = dynamic_cast<TeleportArea*>(n);
            if (!area) continue;
            glm::vec3 hit;
            if (area->raycast(rayOrigin_, rayDir_, maxDistance, hit)) {
                const float d = glm::distance(rayOrigin_, hit);
                if (d < bestDist) {
                    bestDist = d;
                    target_ = hit;
                    hasTarget_ = true;
                    targetArea_ = area;
                }
            }
        }
    } else if (aiming_) {
        // Stick released → commit the teleport to the last valid target.
        if (hasTarget_) commitTeleport();
        aiming_ = false;
        hasTarget_ = false;
        targetArea_ = nullptr;
    }
}

void XRRayInteractor::commitTeleport() {
    SceneTree* t = tree();
    if (!t) return;

    XROrigin* origin = dynamic_cast<XROrigin*>(t->firstInGroup(kXROriginGroup));
    if (!origin) {
        Log::warn("XRRayInteractor: no XROrigin in the scene to teleport");
        return;
    }
    origin->teleportTo(target_);

    // Re-validate the area is still alive (it may have been freed mid-aim) before
    // emitting its landing signal — no dangling pointer deref.
    for (Node* n : t->group(kXRTeleportAreaGroup)) {
        if (n == static_cast<Node*>(targetArea_)) {
            targetArea_->teleported.emit(target_);
            break;
        }
    }
    teleported.emit(target_);
}

void XRRayInteractor::save(nlohmann::json& j) const {
    j["maxDistance"] = maxDistance;
    j["aimThreshold"] = aimThreshold;
}

void XRRayInteractor::load(const nlohmann::json& j) {
    if (j.contains("maxDistance")) maxDistance = j["maxDistance"].get<float>();
    if (j.contains("aimThreshold")) aimThreshold = j["aimThreshold"].get<float>();
}

} // namespace saida
