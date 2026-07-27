#include "ui/RmlUiRenderInterface.hpp"

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "ui/RmlUiRuntime.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace saida {

namespace {

constexpr uint8_t kTransparent = 0;
constexpr uint8_t kOpaque = 255;
constexpr float kPixelCenter = 0.5f;

uint8_t clampByte(float value) {
    return static_cast<uint8_t>(std::clamp(value, 0.0f, 255.0f));
}

float edge(const Rml::Vector2f& a, const Rml::Vector2f& b, const Rml::Vector2f& p) {
    return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);
}

std::string resolveTexturePath(const std::string& source) {
    std::string normalized = pathFromFileUrl(source);
    std::filesystem::path candidate(normalized);
    if (candidate.is_absolute() && std::filesystem::exists(candidate)) return candidate.string();
    if (std::filesystem::exists(candidate)) return std::filesystem::absolute(candidate).string();

    // Images referenced by a UI document belong to the loaded project, not to
    // the engine root assetPath() points at in the editor.
    std::filesystem::path contentCandidate(projectContentPath(normalized));
    if (std::filesystem::exists(contentCandidate)) return contentCandidate.string();

    return normalized;
}

Rml::Vector2f translated(const Rml::Vertex& v, Rml::Vector2f t) {
    return {v.position.x + t.x, v.position.y + t.y};
}

} // namespace

void RmlUiRenderInterface::beginFrame(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    const size_t byteCount = static_cast<size_t>(width_) * height_ * 4;
    premultipliedPixels_.resize(byteCount);
    std::fill(premultipliedPixels_.begin(), premultipliedPixels_.end(), kTransparent);
    outputPixels_.resize(byteCount);
    scissorEnabled_ = false;
    scissorRegion_ = Rml::Rectanglei::MakeInvalid();
    transformEnabled_ = false;
    transform_ = Rml::Matrix4f::Identity();
    rendering_ = true;
}

void RmlUiRenderInterface::endFrame() {
    for (size_t i = 0; i + 3 < premultipliedPixels_.size(); i += 4) {
        uint8_t a = premultipliedPixels_[i + 3];
        outputPixels_[i + 3] = a;
        if (a == 0) {
            outputPixels_[i + 0] = 0;
            outputPixels_[i + 1] = 0;
            outputPixels_[i + 2] = 0;
            continue;
        }

        const float invAlpha = 255.0f / static_cast<float>(a);
        outputPixels_[i + 0] = clampByte(static_cast<float>(premultipliedPixels_[i + 0]) * invAlpha);
        outputPixels_[i + 1] = clampByte(static_cast<float>(premultipliedPixels_[i + 1]) * invAlpha);
        outputPixels_[i + 2] = clampByte(static_cast<float>(premultipliedPixels_[i + 2]) * invAlpha);
    }
    rendering_ = false;
}

Rml::CompiledGeometryHandle RmlUiRenderInterface::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
    Rml::CompiledGeometryHandle handle = nextHandle_++;
    Geometry geometry;
    geometry.vertices.assign(vertices.begin(), vertices.end());
    geometry.indices.assign(indices.begin(), indices.end());
    geometries_[handle] = std::move(geometry);
    return handle;
}

void RmlUiRenderInterface::RenderGeometry(Rml::CompiledGeometryHandle geometryHandle, Rml::Vector2f translation, Rml::TextureHandle texture) {
    if (!rendering_) return;

    auto it = geometries_.find(geometryHandle);
    if (it == geometries_.end()) return;

    const Geometry& geometry = it->second;
    for (size_t i = 0; i + 2 < geometry.indices.size(); i += 3) {
        int ia = geometry.indices[i + 0];
        int ib = geometry.indices[i + 1];
        int ic = geometry.indices[i + 2];
        if (ia < 0 || ib < 0 || ic < 0) continue;
        if (static_cast<size_t>(ia) >= geometry.vertices.size() ||
            static_cast<size_t>(ib) >= geometry.vertices.size() ||
            static_cast<size_t>(ic) >= geometry.vertices.size()) {
            continue;
        }
        drawTriangle(geometry.vertices[ia], geometry.vertices[ib], geometry.vertices[ic], translation, texture);
    }
}

void RmlUiRenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) {
    geometries_.erase(geometry);
}

Rml::TextureHandle RmlUiRenderInterface::LoadTexture(Rml::Vector2i& textureDimensions, const Rml::String& source) {
    RmlUiRuntime::recordFileDependency(source);

    int width = 0;
    int height = 0;
    int channels = 0;
    std::string path = resolveTexturePath(source);
    stbi_uc* loaded = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!loaded) {
        Log::warn("[RmlUi] texture not found: ", source);
        return addMissingTexturePlaceholder(textureDimensions);
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    std::memcpy(pixels.data(), loaded, pixels.size());
    stbi_image_free(loaded);

    textureDimensions = {width, height};
    return addTexture(textureDimensions, std::move(pixels), false);
}

Rml::TextureHandle RmlUiRenderInterface::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i sourceDimensions) {
    std::vector<uint8_t> pixels(source.begin(), source.end());
    return addTexture(sourceDimensions, std::move(pixels), true);
}

void RmlUiRenderInterface::ReleaseTexture(Rml::TextureHandle texture) {
    textures_.erase(texture);
}

void RmlUiRenderInterface::EnableScissorRegion(bool enable) {
    scissorEnabled_ = enable;
}

void RmlUiRenderInterface::SetScissorRegion(Rml::Rectanglei region) {
    scissorRegion_ = region;
}

void RmlUiRenderInterface::SetTransform(const Rml::Matrix4f* transform) {
    transformEnabled_ = transform != nullptr;
    transform_ = transform ? *transform : Rml::Matrix4f::Identity();
}

RmlUiRenderInterface::Pixel RmlUiRenderInterface::sampleTexture(Rml::TextureHandle texture, float u, float v) const {
    if (texture == 0) return {kOpaque, kOpaque, kOpaque, kOpaque};

    auto it = textures_.find(texture);
    if (it == textures_.end() || it->second.size.x <= 0 || it->second.size.y <= 0) {
        return {kOpaque, kOpaque, kOpaque, kOpaque};
    }

    const TextureData& data = it->second;
    float px = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(data.size.x - 1);
    float py = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(data.size.y - 1);
    int x0 = static_cast<int>(std::floor(px));
    int y0 = static_cast<int>(std::floor(py));
    int x1 = std::min(x0 + 1, data.size.x - 1);
    int y1 = std::min(y0 + 1, data.size.y - 1);
    float tx = px - static_cast<float>(x0);
    float ty = py - static_cast<float>(y0);

    auto sample = [&](int x, int y, int channel) -> float {
        size_t index = (static_cast<size_t>(y) * data.size.x + x) * 4 + channel;
        return static_cast<float>(data.premultipliedRgba[index]);
    };

    Pixel out;
    for (int c = 0; c < 4; ++c) {
        float top = sample(x0, y0, c) * (1.0f - tx) + sample(x1, y0, c) * tx;
        float bottom = sample(x0, y1, c) * (1.0f - tx) + sample(x1, y1, c) * tx;
        reinterpret_cast<uint8_t*>(&out)[c] = clampByte(top * (1.0f - ty) + bottom * ty);
    }
    return out;
}

Rml::Vector2f RmlUiRenderInterface::transformPoint(Rml::Vector2f point) const {
    if (!transformEnabled_) return point;
    Rml::Vector4f p(point.x, point.y, 0.0f, 1.0f);
    Rml::Vector4f out = transform_ * p;
    if (std::abs(out.w) > 0.0001f) {
        return {out.x / out.w, out.y / out.w};
    }
    return {out.x, out.y};
}

void RmlUiRenderInterface::drawTriangle(const Rml::Vertex& a, const Rml::Vertex& b, const Rml::Vertex& c,
                                        Rml::Vector2f translation, Rml::TextureHandle texture) {
    Rml::Vector2f p0 = transformPoint(translated(a, translation));
    Rml::Vector2f p1 = transformPoint(translated(b, translation));
    Rml::Vector2f p2 = transformPoint(translated(c, translation));

    float area = edge(p0, p1, p2);
    if (std::abs(area) < 0.0001f) return;

    int minX = static_cast<int>(std::floor(std::min({p0.x, p1.x, p2.x})));
    int minY = static_cast<int>(std::floor(std::min({p0.y, p1.y, p2.y})));
    int maxX = static_cast<int>(std::ceil(std::max({p0.x, p1.x, p2.x})));
    int maxY = static_cast<int>(std::ceil(std::max({p0.y, p1.y, p2.y})));

    minX = std::clamp(minX, 0, static_cast<int>(width_) - 1);
    minY = std::clamp(minY, 0, static_cast<int>(height_) - 1);
    maxX = std::clamp(maxX, 0, static_cast<int>(width_) - 1);
    maxY = std::clamp(maxY, 0, static_cast<int>(height_) - 1);

    if (scissorEnabled_ && scissorRegion_.Valid()) {
        minX = std::max(minX, scissorRegion_.Left());
        minY = std::max(minY, scissorRegion_.Top());
        maxX = std::min(maxX, scissorRegion_.Right() - 1);
        maxY = std::min(maxY, scissorRegion_.Bottom() - 1);
    }

    if (minX > maxX || minY > maxY) return;

    const float invArea = 1.0f / area;
    const bool textured = texture != 0;
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            Rml::Vector2f p{static_cast<float>(x) + kPixelCenter, static_cast<float>(y) + kPixelCenter};
            float w0 = edge(p1, p2, p) * invArea;
            float w1 = edge(p2, p0, p) * invArea;
            float w2 = edge(p0, p1, p) * invArea;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

            Pixel texel{kOpaque, kOpaque, kOpaque, kOpaque};
            if (textured) {
                texel = sampleTexture(texture,
                    a.tex_coord.x * w0 + b.tex_coord.x * w1 + c.tex_coord.x * w2,
                    a.tex_coord.y * w0 + b.tex_coord.y * w1 + c.tex_coord.y * w2);
            }

            Pixel color;
            color.r = clampByte(a.colour.red * w0 + b.colour.red * w1 + c.colour.red * w2);
            color.g = clampByte(a.colour.green * w0 + b.colour.green * w1 + c.colour.green * w2);
            color.b = clampByte(a.colour.blue * w0 + b.colour.blue * w1 + c.colour.blue * w2);
            color.a = clampByte(a.colour.alpha * w0 + b.colour.alpha * w1 + c.colour.alpha * w2);

            // Vertex colours and texels are both premultiplied (Rml::Vertex uses
            // ColourbPremultiplied): modulation is a plain component-wise product.
            Pixel src;
            src.a = static_cast<uint8_t>((static_cast<uint16_t>(texel.a) * color.a) / 255u);
            src.r = static_cast<uint8_t>((static_cast<uint16_t>(texel.r) * color.r) / 255u);
            src.g = static_cast<uint8_t>((static_cast<uint16_t>(texel.g) * color.g) / 255u);
            src.b = static_cast<uint8_t>((static_cast<uint16_t>(texel.b) * color.b) / 255u);
            blendPixel(x, y, src);
        }
    }
}

void RmlUiRenderInterface::blendPixel(int x, int y, Pixel src) {
    size_t index = (static_cast<size_t>(y) * width_ + static_cast<size_t>(x)) * 4;
    const uint16_t invSrcAlpha = 255u - src.a;
    premultipliedPixels_[index + 0] = static_cast<uint8_t>(std::min<uint16_t>(255u, src.r + (premultipliedPixels_[index + 0] * invSrcAlpha) / 255u));
    premultipliedPixels_[index + 1] = static_cast<uint8_t>(std::min<uint16_t>(255u, src.g + (premultipliedPixels_[index + 1] * invSrcAlpha) / 255u));
    premultipliedPixels_[index + 2] = static_cast<uint8_t>(std::min<uint16_t>(255u, src.b + (premultipliedPixels_[index + 2] * invSrcAlpha) / 255u));
    premultipliedPixels_[index + 3] = static_cast<uint8_t>(std::min<uint16_t>(255u, src.a + (premultipliedPixels_[index + 3] * invSrcAlpha) / 255u));
}

// Same visual contract as ResourceManager::missingTexture: a missing UI
// image renders the magenta checkerboard instead of a silent white quad.
Rml::TextureHandle RmlUiRenderInterface::addMissingTexturePlaceholder(Rml::Vector2i& textureDimensions) {
    constexpr int kExtent = 16;
    constexpr int kCell = 4;
    std::vector<uint8_t> checker(static_cast<size_t>(kExtent) * kExtent * 4);
    for (int y = 0; y < kExtent; ++y) {
        for (int x = 0; x < kExtent; ++x) {
            const bool magenta = ((x / kCell) + (y / kCell)) % 2 == 0;
            const size_t i = (static_cast<size_t>(y) * kExtent + x) * 4;
            checker[i + 0] = magenta ? 255 : 24;
            checker[i + 1] = magenta ? 0 : 24;
            checker[i + 2] = magenta ? 255 : 24;
            checker[i + 3] = 255;
        }
    }
    textureDimensions = {kExtent, kExtent};
    return addTexture(textureDimensions, std::move(checker), false);
}

Rml::TextureHandle RmlUiRenderInterface::addTexture(Rml::Vector2i size, std::vector<uint8_t> pixels, bool premultiplied) {
    if (size.x <= 0 || size.y <= 0 || pixels.size() != static_cast<size_t>(size.x) * size.y * 4) {
        return 0;
    }

    if (!premultiplied) {
        for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
            uint16_t alpha = pixels[i + 3];
            pixels[i + 0] = static_cast<uint8_t>((pixels[i + 0] * alpha) / 255u);
            pixels[i + 1] = static_cast<uint8_t>((pixels[i + 1] * alpha) / 255u);
            pixels[i + 2] = static_cast<uint8_t>((pixels[i + 2] * alpha) / 255u);
        }
    }

    Rml::TextureHandle handle = nextHandle_++;
    textures_[handle] = TextureData{size, std::move(pixels)};
    return handle;
}

} // namespace saida
