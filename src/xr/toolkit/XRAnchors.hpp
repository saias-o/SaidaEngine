#pragma once

#include "xr/toolkit/XRTypes.hpp"

#include <cstdint>

namespace saida {

// Backend that actually creates / locates / destroys spatial anchors. Implemented
// by the OpenXR layer when an anchor extension is available
// (XR_MSFT_spatial_anchor, XR_FB_spatial_entity, …) and installed via
// XRAnchors::setBackend(). Absent → anchors simply keep their authored pose.
struct XRAnchorBackend {
    virtual ~XRAnchorBackend() = default;
    virtual uint64_t create(const XRPose& pose) = 0;   // 0 = failure
    virtual bool locate(uint64_t handle, XRPose& out) = 0;
    virtual void destroy(uint64_t handle) = 0;
};

// Global anchor service (the seam). With no backend installed, create() returns 0
// and XRAnchor nodes fall back to their authored transform — so the toolkit
// compiles and behaves sanely before the OpenXR anchor extensions are wired.
class XRAnchors {
public:
    static void setBackend(XRAnchorBackend* backend);  // null to uninstall
    static bool hasBackend();

    static uint64_t create(const XRPose& pose);
    static bool locate(uint64_t handle, XRPose& out);
    static void destroy(uint64_t handle);
};

} // namespace saida
