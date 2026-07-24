#pragma once

namespace saida {

// Global passthrough (AR see-through) toggle — a service like XRInput/Time.
// Gameplay requests it via setEnabled(); the OpenXR session reports whether the
// runtime actually supports it (setSupported) and reads enabled() each frame to
// pick the environment blend mode. When enabled, the XR renderer clears with a
// transparent background and skips the skybox so the real world shows through.
class XRPassthrough {
public:
    // ---- Consumer / gameplay ----
    static void setEnabled(bool e);
    static bool wanted();      // requested by gameplay (independent of support)
    static bool enabled();     // wanted() && supported() — actually compositing
    static bool supported();   // the runtime can do passthrough

    // ---- Producer (OpenXR session) ----
    static void setSupported(bool s);
};

} // namespace saida
