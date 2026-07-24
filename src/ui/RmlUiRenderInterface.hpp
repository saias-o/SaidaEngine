#pragma once

#include <RmlUi/Core/RenderInterface.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace saida {

class RmlUiRenderInterface final : public Rml::RenderInterface {
public:
    void beginFrame(uint32_t width, uint32_t height);
    void endFrame();

    const std::vector<uint8_t>& pixels() const { return outputPixels_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& textureDimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i sourceDimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    void SetTransform(const Rml::Matrix4f* transform) override;

private:
    struct Geometry {
        std::vector<Rml::Vertex> vertices;
        std::vector<int> indices;
    };

    struct TextureData {
        Rml::Vector2i size{0, 0};
        std::vector<uint8_t> premultipliedRgba;
    };

    struct Pixel {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        uint8_t a = 0;
    };

    Pixel sampleTexture(Rml::TextureHandle texture, float u, float v) const;
    Rml::Vector2f transformPoint(Rml::Vector2f point) const;
    void drawTriangle(const Rml::Vertex& a, const Rml::Vertex& b, const Rml::Vertex& c,
                      Rml::Vector2f translation, Rml::TextureHandle texture);
    void blendPixel(int x, int y, Pixel src);
    Rml::TextureHandle addMissingTexturePlaceholder(Rml::Vector2i& textureDimensions);
    Rml::TextureHandle addTexture(Rml::Vector2i size, std::vector<uint8_t> pixels, bool premultiplied);

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::vector<uint8_t> premultipliedPixels_;
    std::vector<uint8_t> outputPixels_;

    bool scissorEnabled_ = false;
    Rml::Rectanglei scissorRegion_;
    bool transformEnabled_ = false;
    Rml::Matrix4f transform_;
    bool rendering_ = false;

    uintptr_t nextHandle_ = 1;
    std::unordered_map<Rml::CompiledGeometryHandle, Geometry> geometries_;
    std::unordered_map<Rml::TextureHandle, TextureData> textures_;
};

} // namespace saida
