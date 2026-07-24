#include "xr/toolkit/XRPassthrough.hpp"

namespace saida {

namespace {
bool g_wanted = false;
bool g_supported = false;
}

void XRPassthrough::setEnabled(bool e) { g_wanted = e; }
bool XRPassthrough::wanted() { return g_wanted; }
bool XRPassthrough::enabled() { return g_wanted && g_supported; }
bool XRPassthrough::supported() { return g_supported; }
void XRPassthrough::setSupported(bool s) { g_supported = s; }

} // namespace saida
