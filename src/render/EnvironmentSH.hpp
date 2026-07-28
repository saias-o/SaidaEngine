#pragma once

#include <glm/glm.hpp>

#include <string>

namespace saida {

// Order-2 spherical harmonics of an environment map's diffuse irradiance.
//
// Diffuse IBL needs the irradiance arriving at a surface from a whole
// hemisphere, which varies with the surface normal — a floor sees the ground,
// a ceiling sees the sky. Approximating it by sampling a coarse mip of the
// equirectangular map instead returns very nearly the environment's average for
// every normal at once: the scene gains a flat, direction-less wash rather than
// lighting. Nine coefficients reproduce Lambertian irradiance to about 1% (a
// well-known result: it is a very low-pass function of direction), evaluate as a
// handful of multiply-adds in the shader, and restore the horizon.
//
// The coefficients are pre-multiplied by the SH basis constants and by the
// Lambert convolution term A_l/pi, so evaluating them is exactly:
//
//   E(n)/pi = c[0]
//           + c[1]*n.y + c[2]*n.z + c[3]*n.x
//           + c[4]*n.x*n.y + c[5]*n.y*n.z + c[6]*(3*n.z*n.z - 1)
//           + c[7]*n.x*n.z + c[8]*(n.x*n.x - n.y*n.y)
//
// which is the value the shader multiplies by albedo.
struct EnvironmentSH {
    glm::vec4 coefficients[9]{};  // rgb used; w padding for std140
};

// Projects an equirectangular image file (.hdr, .png, …) onto `out`.
//
// The direction convention matches the engine's single environment mapping
// (`environmentUV` in shaders/lighting.glsl):
//   u = atan2(dir.z, dir.x) / 2pi + 0.5,  v = asin(-dir.y) / pi + 0.5
//
// Returns false and leaves `out` untouched when the file cannot be decoded.
bool projectEquirectangularSH(const std::string& path, EnvironmentSH& out);

// A neutral, flat environment: the l=0 term only. Used when there is no skybox,
// so the shader path stays the same rather than branching.
EnvironmentSH uniformEnvironmentSH(const glm::vec3& radiance);

} // namespace saida
