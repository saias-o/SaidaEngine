#include "ui/HudRasterizer.hpp"

#include "core/Log.hpp"
#include "nodes/UICanvasNode.hpp"
#include "nodes/UINode.hpp"
#include "nodes/UITextNode.hpp"
#include "ui/RmlUiRenderInterface.hpp"
#include "ui/RmlUiRuntime.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace saida {

namespace {

std::string escapeRmlText(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        default: escaped += c; break;
        }
    }
    return escaped;
}

int toByte(float channel) {
    return static_cast<int>(std::round(std::clamp(channel, 0.0f, 1.0f) * 255.0f));
}

// Emit one absolutely-positioned <div> per active UITextNode descendant and
// count how many were written, so an empty HUD can skip its draw.
void appendTextElements(Node& parent, std::ostringstream& out, int& textCount) {
    for (const auto& child : parent.children()) {
        if (!child->isActiveInHierarchy()) continue;
        if (auto* text = dynamic_cast<UITextNode*>(child.get())) {
            float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
            text->getGlobalRect(x, y, width, height);
            const glm::vec4 color = text->color();
            out << "<div style=\"position:absolute;left:" << x << "px;top:" << y
                << "px;width:" << width << "px;height:" << height
                << "px;font-family:NextSans;font-size:" << text->fontSize()
                << "px;line-height:" << height << "px;color:rgba("
                << toByte(color.r) << ',' << toByte(color.g) << ','
                << toByte(color.b) << ',' << toByte(color.a)
                << ");overflow:hidden;\">"
                << escapeRmlText(text->text()) << "</div>";
            ++textCount;
        }
        appendTextElements(*child, out, textCount);
    }
}

} // namespace

std::string buildHudMarkup(UICanvasNode& canvas, uint32_t width, uint32_t height) {
    for (auto& child : canvas.children()) {
        if (auto* uiChild = dynamic_cast<UINode*>(child.get())) {
            uiChild->updateTransforms(0.0f, 0.0f,
                                      static_cast<float>(width), static_cast<float>(height));
        }
    }

    std::ostringstream markup;
    markup << std::fixed << std::setprecision(3)
           << "<rml><head><style>body{margin:0;width:100%;height:100%;background:transparent;}"
              "div{box-sizing:border-box;}</style></head><body>";
    int textCount = 0;
    appendTextElements(canvas, markup, textCount);
    markup << "</body></rml>";
    return markup.str();
}

uint64_t hudContentHash(const std::string& markup, uint32_t width, uint32_t height) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : markup) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    hash ^= static_cast<uint64_t>(width) << 32;
    hash ^= height;
    return hash;
}

HudRasterizer::~HudRasterizer() {
    if (context_) {
        context_->UnloadAllDocuments();
        context_->Update();
        Rml::RemoveContext(contextName_);
    }
}

void HudRasterizer::ensureContext(uint32_t width, uint32_t height) {
    if (!context_) {
        std::ostringstream name;
        name << "SaidaHud_" << reinterpret_cast<uintptr_t>(this);
        contextName_ = name.str();
        context_ = Rml::CreateContext(contextName_,
                                      {static_cast<int>(width), static_cast<int>(height)},
                                      RmlUiRuntime::renderInterface());
        contentHash_ = 0;
    } else if (width_ != width || height_ != height) {
        context_->SetDimensions({static_cast<int>(width), static_cast<int>(height)});
        contentHash_ = 0;
    }
    width_ = width;
    height_ = height;
}

HudRasterizer::Frame HudRasterizer::rasterize(UICanvasNode& canvas, glm::vec2 viewportSize) {
    Frame frame;
    const uint32_t width = std::max(1u, static_cast<uint32_t>(std::round(viewportSize.x)));
    const uint32_t height = std::max(1u, static_cast<uint32_t>(std::round(viewportSize.y)));

    if (!RmlUiRuntime::ensureInitialized()) return frame;

    const std::string markup = buildHudMarkup(canvas, width, height);
    const bool hasContent = markup.find("<div") != std::string::npos;
    const uint64_t hash = hudContentHash(markup, width, height);

    ensureContext(width, height);
    if (!context_) {
        Log::error("[HUD] failed to create the RmlUi context");
        return frame;
    }

    frame.width = width;
    frame.height = height;
    frame.hasContent = hasContent;

    if (hash == contentHash_ && document_ != nullptr) {
        frame.changed = false;
        return frame;  // unchanged: caller keeps its cached texture
    }

    if (document_) {
        context_->UnloadDocument(document_);
        document_ = nullptr;
    }
    document_ = context_->LoadDocumentFromMemory(markup, "<saida-hud>");
    if (!document_) {
        Log::error("[HUD] failed to build the HUD document");
        return frame;
    }
    document_->Show();
    contentHash_ = hash;
    hasContent_ = hasContent;

    context_->Update();
    RmlUiRenderInterface* rasterizer = RmlUiRuntime::renderer();
    if (!rasterizer) return frame;
    rasterizer->beginFrame(width, height);
    context_->Render();
    rasterizer->endFrame();
    if (rasterizer->pixels().empty()) return frame;

    if (!loggedRaster_) {
        size_t visible = 0;
        for (size_t i = 3; i < rasterizer->pixels().size(); i += 4) {
            if (rasterizer->pixels()[i] != 0) ++visible;
        }
        Log::info("[HUD] rasterized ", visible, " visible pixel(s) at ", width, "x", height);
        loggedRaster_ = true;
    }

    frame.pixels = &rasterizer->pixels();
    frame.changed = true;
    return frame;
}

} // namespace saida
