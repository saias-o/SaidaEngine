#pragma once

#include "core/FileWatcher.hpp"
#include "graphics/Texture.hpp"  // fwd-declares rhi::vulkan::CommandEncoder
#include "scene/Node.hpp"

#include <memory>
#include <string>
#include <vector>

#include <quickjs.h>

namespace saida {

class JsContext;
class Buffer;
class ResourceManager;
class VulkanDevice;

} // namespace saida

namespace Rml {
class Context;
class Element;
class ElementDocument;
class EventListener;
namespace Input {
enum KeyIdentifier : unsigned char;
}
}

namespace saida {

class WebCanvasNode : public Node {
public:
    enum class Mode {
        ScreenSpace,
        WorldSpace
    };

    enum class MouseEvent {
        Down,
        Up,
        Move
    };

    enum class MouseButton {
        None,
        Left,
        Right,
        Middle
    };

    enum class TouchEvent {
        Start,
        Move,
        End,
        Cancel
    };

    WebCanvasNode();
    ~WebCanvasNode() override;

    void init(VulkanDevice& device, uint32_t width, uint32_t height, Mode mode = Mode::ScreenSpace);

    void loadHTML(const std::string& html);
    void loadURL(const std::string& url);
    void executeJS(const std::string& script);
    bool setElementText(const std::string& id, const std::string& text);
    bool setElementRml(const std::string& id, const std::string& rml);

    bool fireMouseEvent(MouseEvent type, int x, int y, MouseButton button, int modifiers = 0);
    bool fireScrollEvent(float deltaX, float deltaY, int modifiers = 0);
    bool fireKeyEvent(bool down, Rml::Input::KeyIdentifier key, int modifiers = 0);
    bool fireTextInput(uint32_t codepoint);
    bool fireTouchEvent(uint64_t id, glm::vec2 position, TouchEvent type, int modifiers = 0);

    const std::string& url() const { return url_; }
    void setUrl(const std::string& url);
    void reload();
    bool hotReloadEnabled() const { return hotReloadEnabled_; }
    void setHotReloadEnabled(bool enabled) { hotReloadEnabled_ = enabled; }

    Mode mode() const { return mode_; }
    void setMode(Mode mode);

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    void resize(uint32_t width, uint32_t height);
    glm::vec2 screenPosition() const;
    glm::vec2 screenSize() const;
    bool screenContains(glm::vec2 point) const;
    glm::vec2 screenToLocal(glm::vec2 point) const;
    bool hitTest(glm::vec2 local) const;

    float worldWidth() const { return worldWidth_; }
    float worldHeight() const;
    void setWorldWidth(float width) { worldWidth_ = width > 0.0f ? width : 1.0f; }
    bool raycast(glm::vec3 origin, glm::vec3 direction, glm::vec2& local, float& distance) const;
    bool interactive() const { return interactive_; }
    void setInteractive(bool enabled) { interactive_ = enabled; }
    int renderOrder() const { return renderOrder_; }
    void setRenderOrder(int order) { renderOrder_ = order; }

    const std::string& lastError() const { return lastError_; }
    bool lastLoadOk() const { return lastLoadOk_; }

    void addStartupScript(const std::string& script) { startupScripts_.push_back(script); }
    // Replace the whole list (editor undo/redo works on full snapshots). The
    // scripts run at the next document (re)load, so no reload is forced here.
    void setStartupScripts(std::vector<std::string> scripts) { startupScripts_ = std::move(scripts); }
    const std::vector<std::string>& startupScripts() const { return startupScripts_; }

    void updateTextureIfNeededAsync(rhi::vulkan::CommandEncoder& encoder);
    Texture* texture() const { return texture_.get(); }

    uint64_t documentGeneration() const { return documentGeneration_; }
    bool isLiveElement(Rml::Element* element, uint64_t generation) const;
    Rml::Element* findElementById(const std::string& id) const;
    Rml::Element* querySelector(const std::string& selector) const;
    void querySelectorAll(const std::string& selector, std::vector<Rml::Element*>& elements) const;
    void addJsEventListener(Rml::Element* element, const std::string& event, JSContext* ctx, JSValueConst callback);
    void removeJsEventListener(Rml::Element* element, const std::string& event, JSContext* ctx, JSValueConst callback);
    void notifyJsMutation();

    const char* typeName() const override { return "WebCanvasNode"; }
    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;

private:
    void ensureJsContext();
    void ensureRmlContext();
    void markUiDirty();
    void installDocumentBindings();
    bool loadDocumentFromState(bool keepExistingOnFailure = false);
    std::string resolveUrlPath() const;
    void updateDocumentWatchers(const std::vector<std::string>& dependencies);
    void appendDocumentWatchers(const std::vector<std::string>& dependencies);
    void rebuildDocumentWatchers();
    void checkHotReload();
    void runStartupScripts();
    bool runDocumentScripts(std::vector<std::string>* dependencies = nullptr);
    void createPlaceholderTexture();
    void setLastError(std::string error);
    void clearJsEventListeners();

    Mode mode_ = Mode::ScreenSpace;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::string url_;
    std::string html_;

    VulkanDevice* device_ = nullptr;
    std::unique_ptr<Texture> texture_;
    std::unique_ptr<Buffer> stagingBuffer_;
    std::unique_ptr<JsContext> jsContext_;
    std::string rmlContextName_;
    Rml::Context* rmlContext_ = nullptr;
    Rml::ElementDocument* document_ = nullptr;
    bool uiDirty_ = true;
    bool hotReloadEnabled_ = true;
    bool interactive_ = true;
    float worldWidth_ = 1.0f;
    int renderOrder_ = 0;
    float hotReloadTimer_ = 0.0f;
    bool lastLoadOk_ = true;
    bool loggedRenderStats_ = false;
    std::string lastError_;
    uint64_t documentGeneration_ = 1;
    std::vector<std::string> dependencyPaths_;
    std::vector<WatchedFile> documentWatchers_;
    std::vector<std::string> startupScripts_;

    struct JsEventListenerRecord;
    std::vector<std::unique_ptr<JsEventListenerRecord>> jsEventListeners_;
};

} // namespace saida
