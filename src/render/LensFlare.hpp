#pragma once

#include <glm/glm.hpp>

namespace saida {

class Scene;

// The directional light whose source the tonemap draws lens effects for
// (`LightNode::lensFlare`, `LightNode::sunStar`). Where on screen the source
// lands, and how much of it is visible, is the tonemap's business: this only
// says which light, where it is in the world and how bright it is.
struct LensFlareSource {
    glm::vec3 towardLight{0.0f, 1.0f, 0.0f};  // world space, unit, pointing at the source
    glm::vec3 radiance{0.0f};                 // linear colour x intensity
    bool lensFlare = false;
    bool sunStar = false;

    bool active() const { return lensFlare || sunStar; }
};

// The brightest visible directional light that asks for either effect and
// emits something; an inactive source when there is none. A scene holds a
// handful of lights, so this is one short scan a frame.
LensFlareSource pickLensFlareSource(const Scene& scene);

} // namespace saida
