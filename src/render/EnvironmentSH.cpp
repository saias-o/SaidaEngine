#include "render/EnvironmentSH.hpp"

#include "core/Log.hpp"
#include "core/Profiler.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace saida {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Real SH basis constants, l = 0..2.
constexpr float kY00 = 0.282095f;
constexpr float kY1 = 0.488603f;
constexpr float kY2xy = 1.092548f;
constexpr float kY20 = 0.315392f;
constexpr float kY22 = 0.546274f;

// Lambert convolution, already divided by pi so the result is the radiance to
// multiply by albedo rather than the irradiance.
constexpr float kA0 = 1.0f;          // pi     / pi
constexpr float kA1 = 2.0f / 3.0f;   // 2pi/3  / pi
constexpr float kA2 = 0.25f;         // pi/4   / pi

// Projection resolution. The integrand is band-limited to l <= 2, so a small
// image is not an approximation of the projection — it IS the projection, minus
// aliasing that the box downsample below removes anyway. It also keeps a 4K HDRI
// from costing a full decode's worth of work per pixel.
constexpr int kProjectionWidth = 128;
constexpr int kProjectionHeight = 64;

struct Image {
    std::vector<float> pixels;  // rgb, row major
    int width = 0;
    int height = 0;
};

bool loadHdrOrLdr(const std::string& path, Image& out) {
    int w = 0;
    int h = 0;
    int channels = 0;
    const bool isHdr = stbi_is_hdr(path.c_str()) != 0;

    if (isHdr) {
        float* data = stbi_loadf(path.c_str(), &w, &h, &channels, 3);
        if (!data) return false;
        out.pixels.assign(data, data + static_cast<size_t>(w) * h * 3);
        stbi_image_free(data);
    } else {
        stbi_uc* data = stbi_load(path.c_str(), &w, &h, &channels, 3);
        if (!data) return false;
        out.pixels.resize(static_cast<size_t>(w) * h * 3);
        // An LDR environment is authored in sRGB and sampled as sRGB by the GPU;
        // the projection has to work in the same linear space the shader does.
        for (size_t i = 0; i < out.pixels.size(); ++i) {
            const float c = data[i] / 255.0f;
            out.pixels[i] = c <= 0.04045f ? c / 12.92f
                                          : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        stbi_image_free(data);
    }
    out.width = w;
    out.height = h;
    return w > 0 && h > 0;
}

// Box-average the source into the projection grid. Averaging beats point
// sampling here: a single bright sun texel would otherwise be either missed
// entirely or counted as if it filled its whole destination texel.
void downsample(const Image& src, Image& dst) {
    dst.width = std::min(kProjectionWidth, src.width);
    dst.height = std::min(kProjectionHeight, src.height);
    dst.pixels.assign(static_cast<size_t>(dst.width) * dst.height * 3, 0.0f);

    for (int y = 0; y < dst.height; ++y) {
        const int y0 = y * src.height / dst.height;
        const int y1 = std::max(y0 + 1, (y + 1) * src.height / dst.height);
        for (int x = 0; x < dst.width; ++x) {
            const int x0 = x * src.width / dst.width;
            const int x1 = std::max(x0 + 1, (x + 1) * src.width / dst.width);

            float r = 0.0f, g = 0.0f, b = 0.0f;
            for (int sy = y0; sy < y1; ++sy) {
                for (int sx = x0; sx < x1; ++sx) {
                    const size_t i = (static_cast<size_t>(sy) * src.width + sx) * 3;
                    r += src.pixels[i];
                    g += src.pixels[i + 1];
                    b += src.pixels[i + 2];
                }
            }
            const float inv = 1.0f / static_cast<float>((y1 - y0) * (x1 - x0));
            const size_t o = (static_cast<size_t>(y) * dst.width + x) * 3;
            dst.pixels[o] = r * inv;
            dst.pixels[o + 1] = g * inv;
            dst.pixels[o + 2] = b * inv;
        }
    }
}

} // namespace

EnvironmentSH uniformEnvironmentSH(const glm::vec3& radiance) {
    EnvironmentSH sh;
    // A constant environment has only the l=0 band, and its Lambert response is
    // the constant itself: kA0 * kY00 * (radiance * 4pi * kY00) == radiance.
    sh.coefficients[0] = glm::vec4(radiance, 0.0f);
    return sh;
}

bool projectEquirectangularSH(const std::string& path, EnvironmentSH& out) {
    SAIDA_PROFILE_SCOPE("Render/ProjectEnvironmentSH");

    Image source;
    if (!loadHdrOrLdr(path, source)) {
        Log::warn("EnvironmentSH: cannot decode environment '", path,
                  "' — diffuse IBL will stay flat");
        return false;
    }

    Image image;
    downsample(source, image);

    glm::vec3 acc[9] = {};
    const float dPhi = 2.0f * kPi / static_cast<float>(image.width);
    const float dTheta = kPi / static_cast<float>(image.height);

    for (int y = 0; y < image.height; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(image.height);
        // Inverse of the engine's environmentUV: v maps to asin(-dir.y).
        const float elevation = (v - 0.5f) * kPi;
        const float dirY = -std::sin(elevation);
        const float ring = std::cos(elevation);
        // Solid angle of a texel on this row.
        const float weight = dPhi * dTheta * ring;

        for (int x = 0; x < image.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(image.width);
            const float phi = (u - 0.5f) * 2.0f * kPi;
            const glm::vec3 d(ring * std::cos(phi), dirY, ring * std::sin(phi));

            const size_t i = (static_cast<size_t>(y) * image.width + x) * 3;
            const glm::vec3 c(image.pixels[i], image.pixels[i + 1], image.pixels[i + 2]);
            const glm::vec3 cw = c * weight;

            acc[0] += cw * kY00;
            acc[1] += cw * (kY1 * d.y);
            acc[2] += cw * (kY1 * d.z);
            acc[3] += cw * (kY1 * d.x);
            acc[4] += cw * (kY2xy * d.x * d.y);
            acc[5] += cw * (kY2xy * d.y * d.z);
            acc[6] += cw * (kY20 * (3.0f * d.z * d.z - 1.0f));
            acc[7] += cw * (kY2xy * d.x * d.z);
            acc[8] += cw * (kY22 * (d.x * d.x - d.y * d.y));
        }
    }

    // Fold the basis constant and the Lambert term in, so the shader evaluates a
    // plain polynomial in the normal.
    const float scale[9] = {
        kA0 * kY00,
        kA1 * kY1, kA1 * kY1, kA1 * kY1,
        kA2 * kY2xy, kA2 * kY2xy, kA2 * kY20, kA2 * kY2xy, kA2 * kY22,
    };
    for (int i = 0; i < 9; ++i)
        out.coefficients[i] = glm::vec4(acc[i] * scale[i], 0.0f);

    return true;
}

} // namespace saida
