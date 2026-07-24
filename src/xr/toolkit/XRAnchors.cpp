#include "xr/toolkit/XRAnchors.hpp"

namespace saida {

namespace {
XRAnchorBackend* g_backend = nullptr;
}

void XRAnchors::setBackend(XRAnchorBackend* backend) { g_backend = backend; }
bool XRAnchors::hasBackend() { return g_backend != nullptr; }

uint64_t XRAnchors::create(const XRPose& pose) {
    return g_backend ? g_backend->create(pose) : 0;
}

bool XRAnchors::locate(uint64_t handle, XRPose& out) {
    return g_backend && handle ? g_backend->locate(handle, out) : false;
}

void XRAnchors::destroy(uint64_t handle) {
    if (g_backend && handle) g_backend->destroy(handle);
}

} // namespace saida
