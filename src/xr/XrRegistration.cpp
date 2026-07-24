#include "xr/XrRegistration.hpp"

#include "scene/BehaviourRegistry.hpp"
#include "scene/NodeRegistry.hpp"

#include "xr/toolkit/XRGrabbable.hpp"
#include "xr/toolkit/XRTouchable.hpp"
#include "xr/toolkit/XRDirectInteractor.hpp"
#include "xr/toolkit/XRRayInteractor.hpp"

#include "xr/toolkit/XRController.hpp"
#include "xr/toolkit/XRHand.hpp"
#include "xr/toolkit/XROrigin.hpp"
#include "xr/toolkit/TeleportArea.hpp"
#include "xr/toolkit/XRAnchor.hpp"

namespace saida::xr {

void registerTypes() {
    // Register behaviours
    BehaviourRegistry::instance().registerType<XRGrabbable>("XRGrabbable");
    BehaviourRegistry::instance().registerType<XRTouchable>("XRTouchable");
    BehaviourRegistry::instance().registerType<XRDirectInteractor>("XRDirectInteractor");
    BehaviourRegistry::instance().registerType<XRRayInteractor>("XRRayInteractor");

    // Register nodes
    NodeRegistry::instance().registerType<XRController>("XRController");
    NodeRegistry::instance().registerType<XRHandNode>("XRHand");
    NodeRegistry::instance().registerType<XROrigin>("XROrigin");
    NodeRegistry::instance().registerType<TeleportArea>("TeleportArea");
    NodeRegistry::instance().registerType<XRAnchor>("XRAnchor");
}

} // namespace saida::xr
