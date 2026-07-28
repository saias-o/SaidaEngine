#include "nodes/UICanvasNode.hpp"
#include "nodes/UITextNode.hpp"
#include "ui/HudRasterizer.hpp"
#include "ui/RmlUiRenderInterface.hpp"
#include "ui/RmlUiRuntime.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// UI corpus V1: the CPU RmlUi backend renders without a GPU, so every
// rendering contract (geometry, blend, textures, project stylesheet,
// clipping, transforms, resize, DPI) is proven here by directly reading
// the rasterized pixels.

using namespace saida;
namespace fs = std::filesystem;

namespace {

int gChecks = 0;

void require(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        std::cerr << "[ui-corpus] FAIL: " << what << "\n";
        std::abort();
    }
}

struct Raster {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;

    struct Rgba {
        uint8_t r = 0, g = 0, b = 0, a = 0;
    };

    Rgba at(uint32_t x, uint32_t y) const {
        const size_t i = (static_cast<size_t>(y) * width + x) * 4;
        return {pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
    }

    size_t visibleInRect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) const {
        size_t count = 0;
        for (uint32_t y = y0; y < y1 && y < height; ++y) {
            for (uint32_t x = x0; x < x1 && x < width; ++x) {
                if (at(x, y).a != 0) ++count;
            }
        }
        return count;
    }
};

bool near(uint8_t value, int expected, int tolerance = 8) {
    return std::abs(static_cast<int>(value) - expected) <= tolerance;
}

void requirePixel(const Raster& raster, uint32_t x, uint32_t y,
                  int r, int g, int b, int a, const char* what, int tolerance = 8) {
    const Raster::Rgba actual = raster.at(x, y);
    const bool ok = near(actual.r, r, tolerance) && near(actual.g, g, tolerance) &&
                    near(actual.b, b, tolerance) && near(actual.a, a, tolerance);
    if (!ok) {
        std::cerr << "[ui-corpus] pixel (" << x << "," << y << ") = rgba("
                  << static_cast<int>(actual.r) << "," << static_cast<int>(actual.g) << ","
                  << static_cast<int>(actual.b) << "," << static_cast<int>(actual.a)
                  << "), expected rgba(" << r << "," << g << "," << b << "," << a << ")\n";
    }
    require(ok, what);
}

Raster renderContext(Rml::Context& context, uint32_t width, uint32_t height) {
    RmlUiRenderInterface* renderer = RmlUiRuntime::renderer();
    require(renderer != nullptr, "renderer available");
    context.Update();
    renderer->beginFrame(width, height);
    context.Render();
    renderer->endFrame();
    Raster raster;
    raster.width = width;
    raster.height = height;
    raster.pixels = renderer->pixels();
    require(raster.pixels.size() == static_cast<size_t>(width) * height * 4,
            "raster has width*height*4 bytes");
    return raster;
}

class ContextFixture {
public:
    ContextFixture(const char* name, int width, int height) {
        require(RmlUiRuntime::ensureInitialized(), "RmlUi initializes headless");
        context_ = Rml::CreateContext(name, {width, height}, RmlUiRuntime::renderInterface());
        require(context_ != nullptr, "context created");
        name_ = name;
    }

    ~ContextFixture() {
        context_->UnloadAllDocuments();
        context_->Update();
        Rml::RemoveContext(name_);
    }

    Rml::Context& operator*() { return *context_; }
    Rml::Context* operator->() { return context_; }

    Rml::ElementDocument* show(const std::string& markup) {
        Rml::ElementDocument* document = context_->LoadDocumentFromMemory(markup, "<ui-corpus>");
        if (document) {
            document->Show();
            context_->Update();
        }
        return document;
    }

    Rml::ElementDocument* showFile(const fs::path& path) {
        Rml::ElementDocument* document = context_->LoadDocument(path.generic_string());
        if (document) {
            document->Show();
            context_->Update();
        }
        return document;
    }

private:
    Rml::Context* context_ = nullptr;
    std::string name_;
};

fs::path freshSandbox(const char* label) {
    const fs::path root = fs::temp_directory_path() / "SaidaUiCorpusTests" / label;
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void writeFile(const fs::path& path, const std::string& contents) {
    std::ofstream file(path, std::ios::binary);
    file << contents;
}

// Binary P6 PPM: decoded by stb_image without depending on a PNG encoder.
void writeSolidPpm(const fs::path& path, int width, int height,
                   uint8_t r, uint8_t g, uint8_t b) {
    std::ofstream file(path, std::ios::binary);
    file << "P6\n" << width << " " << height << "\n255\n";
    for (int i = 0; i < width * height; ++i) {
        file.put(static_cast<char>(r));
        file.put(static_cast<char>(g));
        file.put(static_cast<char>(b));
    }
}

const char* kDocumentShell =
    "<rml><head><style>"
    "body { margin: 0; width: 100%; height: 100%; }"
    "%s"
    "</style></head><body>%s</body></rml>";

std::string document(const std::string& style, const std::string& body) {
    std::string out = kDocumentShell;
    out.replace(out.find("%s"), 2, style);
    out.replace(out.find("%s"), 2, body);
    return out;
}

// -- Solid geometry, alpha blending, and transparent background --------------

void testSolidGeometryAndBlend() {
    ContextFixture ctx("corpus-blend", 256, 256);
    require(ctx.show(document(
        "#red { position: absolute; left: 10px; top: 10px; width: 100px; height: 100px;"
        "       background-color: #FF0000; }"
        "#blue { position: absolute; left: 60px; top: 10px; width: 100px; height: 100px;"
        "        background-color: #0000FF80; }",
        "<div id=\"red\"/><div id=\"blue\"/>")) != nullptr, "blend document loads");

    Raster raster = renderContext(*ctx, 256, 256);

    requirePixel(raster, 30, 60, 255, 0, 0, 255, "pure red region is opaque red");
    requirePixel(raster, 100, 60, 127, 0, 128, 255,
                 "half-alpha blue over red blends to half red / half blue");

    Raster::Rgba blueOnly = raster.at(140, 60);
    require(near(blueOnly.b, 255) && near(blueOnly.a, 128),
            "half-alpha blue over nothing keeps its own alpha");

    require(raster.at(220, 220).a == 0, "outside all geometry stays fully transparent");
}

// -- Text: default font glyphs produce pixels --------------------------------

void testTextRendersGlyphs() {
    ContextFixture ctx("corpus-text", 256, 128);
    require(ctx.show(document(
        "#label { position: absolute; left: 8px; top: 8px; width: 240px; height: 48px;"
        "         font-family: Arial; font-size: 32px; color: #FFFFFF; }",
        "<div id=\"label\">Corpus</div>")) != nullptr, "text document loads");

    Raster raster = renderContext(*ctx, 256, 128);
    size_t glyphPixels = raster.visibleInRect(8, 8, 248, 56);
    require(glyphPixels > 100, "glyphs rasterize visible pixels");
    require(raster.visibleInRect(0, 80, 256, 128) == 0, "no stray pixels below the label");
}

// -- Project stylesheet loaded from disk, unsupported property filtered ------

void testStylesheetFromFile() {
    const fs::path sandbox = freshSandbox("stylesheet");
    // text-shadow (unsupported) precedes background-color: the filter must
    // neutralize the first declaration without losing the next one.
    writeFile(sandbox / "menu.rcss",
              "#panel { position: absolute; left: 0px; top: 0px;"
              " width: 64px; height: 64px;"
              " text-shadow: 2px 2px #000000;"
              " background-color: #00FF00; }");
    writeFile(sandbox / "menu.rml",
              "<rml><head>"
              "<link type=\"text/rcss\" href=\"menu.rcss\"/>"
              "<style>body { margin: 0; width: 100%; height: 100%; }</style>"
              "</head><body><div id=\"panel\"/></body></rml>");

    ContextFixture ctx("corpus-stylesheet", 128, 128);
    Rml::ElementDocument* doc = ctx->LoadDocument((sandbox / "menu.rml").generic_string());
    require(doc != nullptr, "document with external stylesheet loads");
    doc->Show();

    Raster raster = renderContext(*ctx, 128, 128);
    Raster::Rgba panel = raster.at(32, 32);
    require(near(panel.g, 255) && near(panel.r, 0) && panel.a == 255,
            "declaration after filtered text-shadow still applies");
    fs::remove_all(sandbox.parent_path());
}

// -- Project image decoded and sampled; missing image is non-fatal ----------

void testImageTexture() {
    const fs::path sandbox = freshSandbox("image");
    writeSolidPpm(sandbox / "sprite.ppm", 4, 4, 0, 255, 0);
    writeFile(sandbox / "hud.rml",
              "<rml><head><style>"
              "body { margin: 0; width: 100%; height: 100%; }"
              "img { position: absolute; }"
              "</style></head><body>"
              "<img src=\"sprite.ppm\" style=\"left: 0px; top: 0px; width: 32px; height: 32px;\"/>"
              "<img src=\"missing.png\" style=\"left: 64px; top: 0px; width: 32px; height: 32px;\"/>"
              "</body></rml>");

    ContextFixture ctx("corpus-image", 128, 64);
    Rml::ElementDocument* doc = ctx->LoadDocument((sandbox / "hud.rml").generic_string());
    require(doc != nullptr, "document with images loads despite one missing image");
    doc->Show();

    Raster raster = renderContext(*ctx, 128, 64);
    Raster::Rgba sprite = raster.at(16, 16);
    require(near(sprite.g, 255) && near(sprite.r, 0) && sprite.a == 255,
            "image file decodes and rasterizes");
    // Engine convention: a missing asset renders the visible magenta checker.
    size_t magenta = 0;
    for (uint32_t y = 0; y < 32; ++y) {
        for (uint32_t x = 64; x < 96; ++x) {
            Raster::Rgba p = raster.at(x, y);
            if (p.r > 200 && p.b > 200 && p.g < 50 && p.a == 255) ++magenta;
        }
    }
    require(magenta > 100, "missing image renders the magenta checker fallback");
    fs::remove_all(sandbox.parent_path());
}

// -- overflow: hidden -> scissor actually applied -----------------------------

void testOverflowClipsChildren() {
    ContextFixture ctx("corpus-clip", 256, 128);
    require(ctx.show(document(
        "#window { position: absolute; left: 20px; top: 20px; width: 60px; height: 60px;"
        "          overflow: hidden; }"
        "#wide { display: block; width: 200px; height: 40px;"
        "        background-color: #FF00FF; }",
        "<div id=\"window\"><div id=\"wide\"/></div>")) != nullptr, "clip document loads");

    Raster raster = renderContext(*ctx, 256, 128);
    require(raster.at(40, 30).a == 255, "child visible inside the clipping parent");
    require(raster.visibleInRect(81, 20, 256, 80) == 0,
            "child pixels outside overflow:hidden are scissored");
}

// -- CSS transform propagated through to the rasterizer -----------------------

void testTransformTranslatesGeometry() {
    ContextFixture ctx("corpus-transform", 256, 128);
    require(ctx.show(document(
        "#box { position: absolute; left: 10px; top: 10px; width: 40px; height: 40px;"
        "       background-color: #FFFF00; transform: translateX(100px); }",
        "<div id=\"box\"/>")) != nullptr, "transform document loads");

    Raster raster = renderContext(*ctx, 256, 128);
    require(raster.visibleInRect(10, 10, 50, 50) == 0,
            "untransformed position is empty");
    Raster::Rgba moved = raster.at(130, 30);
    require(near(moved.r, 255) && near(moved.g, 255) && moved.a == 255,
            "translated geometry lands 100px to the right");
}

// -- Context resize and pixel density (dp) ------------------------------------

void testResizeRelayouts() {
    ContextFixture ctx("corpus-resize", 200, 100);
    require(ctx.show(document(
        "#anchored { position: absolute; right: 0px; top: 0px; width: 10px; height: 100px;"
        "            background-color: #00FFFF; }",
        "<div id=\"anchored\"/>")) != nullptr, "resize document loads");

    Raster raster = renderContext(*ctx, 200, 100);
    require(raster.at(195, 50).a == 255, "right-anchored box sits at the initial edge");

    ctx->SetDimensions({300, 100});
    raster = renderContext(*ctx, 300, 100);
    require(raster.at(295, 50).a == 255, "after resize the box follows the new edge");
    require(raster.visibleInRect(180, 0, 220, 100) == 0, "old edge is empty after resize");
}

void testDpRatioScalesDpUnits() {
    ContextFixture ctx("corpus-dp", 256, 128);
    require(ctx.show(document(
        "#dpbox { position: absolute; left: 0px; top: 0px; width: 50dp; height: 40px;"
        "         background-color: #FFFFFF; }",
        "<div id=\"dpbox\"/>")) != nullptr, "dp document loads");

    Raster raster = renderContext(*ctx, 256, 128);
    require(raster.at(45, 20).a == 255 && raster.visibleInRect(55, 0, 256, 40) == 0,
            "50dp is 50px at dp-ratio 1");

    ctx->SetDensityIndependentPixelRatio(2.0f);
    raster = renderContext(*ctx, 256, 128);
    require(raster.at(95, 20).a == 255, "50dp is 100px at dp-ratio 2");
}

// -- Shared HUD: the rasterizer used by BOTH desktop and Web -----------------
//
// Proves that the shared markup generation + rasterization (HudRasterizer)
// turns a text-bearing UICanvas into visible glyphs. This is the parity
// contract (SPEC section 1): both platforms composite pixels produced by this code.

void testHudRasterizerRendersCanvasText() {
    auto canvas = std::make_unique<UICanvasNode>();
    auto* text = static_cast<UITextNode*>(canvas->addChild(std::make_unique<UITextNode>()));
    text->setText("Relics: 3");
    text->setFontSize(32.0f);
    text->setAnchor(0.0f, 0.0f);   // anchored top-left
    text->setPivot(0.0f, 0.0f);
    text->setPosition(20.0f, 20.0f);
    text->setSize(300.0f, 48.0f);
    text->setColor(glm::vec4(1.0f));

    // The shared markup correctly names the text and its default font.
    const std::string markup = buildHudMarkup(*canvas, 640, 360);
    require(markup.find("Relics: 3") != std::string::npos, "HUD markup contains the text");
    require(markup.find("font-family:NextSans") != std::string::npos,
            "HUD markup uses the engine default font family");

    HudRasterizer hud;
    HudRasterizer::Frame frame = hud.rasterize(*canvas, glm::vec2(640.0f, 360.0f));
    require(frame.hasContent, "HUD with a text node reports content");
    require(frame.changed && frame.pixels != nullptr, "first rasterize produces pixels");
    require(frame.width == 640 && frame.height == 360, "raster matches the viewport size");

    Raster raster;
    raster.width = frame.width;
    raster.height = frame.height;
    raster.pixels = *frame.pixels;
    require(raster.visibleInRect(20, 20, 320, 68) > 100, "HUD glyphs rasterize where the text sits");
    require(raster.visibleInRect(0, 120, 640, 360) == 0, "no HUD pixels away from the text");

    // An unchanged HUD is not re-rasterized (the caller keeps its texture).
    HudRasterizer::Frame again = hud.rasterize(*canvas, glm::vec2(640.0f, 360.0f));
    require(again.hasContent && !again.changed, "unchanged HUD reports no re-raster");

    // A modified text triggers a re-rasterization.
    text->setText("Relics: 4");
    HudRasterizer::Frame changed = hud.rasterize(*canvas, glm::vec2(640.0f, 360.0f));
    require(changed.changed && changed.pixels != nullptr, "changed text re-rasterizes");
}

void testHudRasterizerEmptyCanvasHasNoContent() {
    auto canvas = std::make_unique<UICanvasNode>();
    HudRasterizer hud;
    HudRasterizer::Frame frame = hud.rasterize(*canvas, glm::vec2(320.0f, 240.0f));
    require(!frame.hasContent, "text-less HUD reports no content (no wasted draw)");
}

void testVerticalSliceMenuButtonHoverMatchesVisualBox() {
    ContextFixture ctx("corpus-vertical-slice-menu", 1021, 654);
    const fs::path menuPath =
        fs::path(__FILE__).parent_path().parent_path() / "VerticalSlice/ui/main_menu.html";
    Rml::ElementDocument* document = ctx.showFile(menuPath);
    require(document != nullptr, "VerticalSlice main menu loads in the RmlUi corpus");

    const char* ids[] = {
        "play-button", "how-button", "options-button", "credits-button", "quit-button"
    };
    for (const char* id : ids) {
        Rml::Element* button = document->GetElementById(id);
        require(button != nullptr, "VerticalSlice menu button exists");
        const Rml::Vector2f position = button->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f size = button->GetBox().GetSize(Rml::BoxArea::Border);
        const Rml::Vector2f center = position + size * 0.5f;

        ctx->ProcessMouseMove(static_cast<int>(center.x), static_cast<int>(center.y), 0);
        ctx->Update();
        require(button->IsPseudoClassSet("hover"),
                "pointer at a VerticalSlice button's visual center hovers that button");
    }
}

// -- CPU backend measurement -> GPU backend decision --------------------------
//
// The choice of "no RmlUi GPU backend in V1" (SPEC section 8.3) must rest on a
// measurement, not a guess. We rasterize a realistic HUD (several lines) at
// 1080p, with content changed on every iteration to force a real re-render, and
// publish the per-frame cost. This cost is paid ONLY when the HUD changes (the
// rasterizer skips identical content), not on every frame. The number printed
// here is a Debug measurement (the dev build); the deliverable perf figure is
// the Release hitchMax from the Witness harness (~0.05s under full load). The
// threshold below is just a Debug anti-regression guard, not a perf budget.

void testCpuRasterizationCostIsBudgeted() {
    auto canvas = std::make_unique<UICanvasNode>();
    for (int line = 0; line < 6; ++line) {
        auto* text = static_cast<UITextNode*>(canvas->addChild(std::make_unique<UITextNode>()));
        text->setAnchor(0.0f, 0.0f);
        text->setPivot(0.0f, 0.0f);
        text->setPosition(24.0f, 24.0f + static_cast<float>(line) * 40.0f);
        text->setSize(600.0f, 36.0f);
        text->setFontSize(28.0f);
    }
    auto* firstLine = static_cast<UITextNode*>(canvas->children().front().get());

    HudRasterizer hud;
    constexpr int kFrames = 120;
    // Warm-up: build the context and first document outside the timed loop.
    firstLine->setText("warmup");
    hud.rasterize(*canvas, glm::vec2(1920.0f, 1080.0f));

    const auto start = std::chrono::steady_clock::now();
    int rasterized = 0;
    for (int i = 0; i < kFrames; ++i) {
        firstLine->setText("Score " + std::to_string(i) + "  |  Health 100  |  Ammo 24");
        HudRasterizer::Frame frame = hud.rasterize(*canvas, glm::vec2(1920.0f, 1080.0f));
        if (frame.changed) ++rasterized;
    }
    const auto end = std::chrono::steady_clock::now();

    require(rasterized == kFrames, "each changed HUD re-rasterizes (no stale skip)");
    const double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
    const double perFrameMs = totalMs / kFrames;
    std::cout << "[ui-corpus] CPU HUD raster: " << perFrameMs
              << " ms/frame at 1920x1080 (6 lines, Debug; content-change only)\n";
    // Debug regression guard only (a Release build and content that rarely
    // changes are both far cheaper); the perf basis for deferring a GPU backend
    // is the Witness Release hitchMax, not this Debug number.
    require(perFrameMs < 250.0, "CPU HUD rasterization has not regressed catastrophically");
}

} // namespace

int main() {
    testSolidGeometryAndBlend();
    testTextRendersGlyphs();
    testStylesheetFromFile();
    testImageTexture();
    testOverflowClipsChildren();
    testTransformTranslatesGeometry();
    testResizeRelayouts();
    testDpRatioScalesDpUnits();
    testHudRasterizerRendersCanvasText();
    testHudRasterizerEmptyCanvasHasNoContent();
    testVerticalSliceMenuButtonHoverMatchesVisualBox();
    testCpuRasterizationCostIsBudgeted();

    RmlUiRuntime::shutdown();
    std::cout << "[ui-corpus] PASS (" << gChecks << " checks)\n";
    return 0;
}
