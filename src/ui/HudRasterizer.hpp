#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Rml {
class Context;
class ElementDocument;
} // namespace Rml

namespace saida {

class UICanvasNode;

// Build the RmlUi document for a UICanvas's text HUD. Refreshes the canvas
// children transforms for the target viewport first, then emits one
// absolutely-positioned element per active UITextNode descendant. Desktop and
// the Web player share this builder so both rasterize byte-identical markup —
// the visual-parity invariant (SPEC §1) applied to the HUD.
std::string buildHudMarkup(UICanvasNode& canvas, uint32_t width, uint32_t height);

// Stable content key: same markup at the same size hashes equal, so the
// rasterizer can skip re-rendering an unchanged HUD.
uint64_t hudContentHash(const std::string& markup, uint32_t width, uint32_t height);

// Owns the RmlUi context that turns a UICanvas HUD into an RGBA8 pixel buffer
// with the CPU backend (no GPU). The caller uploads the pixels into whatever
// texture its RHI needs; only that upload is platform-specific.
class HudRasterizer {
public:
    struct Frame {
        const std::vector<uint8_t>* pixels = nullptr;  // RGBA8, width*height*4; null when unavailable
        uint32_t width = 0;
        uint32_t height = 0;
        bool changed = false;     // markup or size differs from the previous rasterize()
        bool hasContent = false;  // the HUD has at least one text element to draw
    };

    HudRasterizer() = default;
    ~HudRasterizer();
    HudRasterizer(const HudRasterizer&) = delete;
    HudRasterizer& operator=(const HudRasterizer&) = delete;

    // Rasterize the canvas HUD at the given viewport size. Returns pixels only
    // when the raster changed (or on first call); an unchanged HUD reports
    // changed=false with the same width/height so the caller keeps its texture.
    Frame rasterize(UICanvasNode& canvas, glm::vec2 viewportSize);

private:
    void ensureContext(uint32_t width, uint32_t height);

    Rml::Context* context_ = nullptr;
    Rml::ElementDocument* document_ = nullptr;
    std::string contextName_;
    uint64_t contentHash_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool hasContent_ = false;
    bool loggedRaster_ = false;
};

} // namespace saida
