#include "nodes/WebCanvasNode.hpp"

#include "core/Profiler.hpp"

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "core/Time.hpp"
#include "graphics/Buffer.hpp"
#include "graphics/ResourceManager.hpp"
#include "graphics/VulkanDevice.hpp"
#include "scripting/JsContext.hpp"
#include "scripting/JsRuntime.hpp"
#include "scene/SceneTree.hpp"
#include "ui/RmlUiRenderInterface.hpp"
#include "ui/RmlUiRuntime.hpp"
#include "ui/WorldPanelGeometry.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Types.h>
#include <quickjs.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <glm/gtc/matrix_inverse.hpp>
#include <regex>
#include <sstream>
#include <vector>

namespace saida {

namespace {
std::atomic<uint64_t> gNextWebCanvasId{1};
constexpr float kHotReloadCheckIntervalSeconds = 0.5f;
JSClassID gElementClassId = 0;
JSClassID gClassListClassId = 0;
JSClassID gStyleClassId = 0;

struct ElementHandle {
    WebCanvasNode* canvas = nullptr;
    Rml::Element* element = nullptr;
    uint64_t generation = 0;
};

std::string jsToString(JSContext* ctx, JSValueConst value) {
    const char* raw = JS_ToCString(ctx, value);
    std::string out = raw ? raw : "";
    JS_FreeCString(ctx, raw);
    return out;
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::filesystem::path resolveDocumentRelative(const std::string& source, const std::string& documentUrl) {
    std::filesystem::path sourcePath(pathFromFileUrl(source));
    if (sourcePath.is_absolute() && std::filesystem::exists(sourcePath)) return sourcePath;

    if (!documentUrl.empty()) {
        std::filesystem::path docPath(pathFromFileUrl(documentUrl));
        if (!docPath.is_absolute()) {
            std::filesystem::path assetDocPath(assetPath(docPath.generic_string()));
            if (std::filesystem::exists(assetDocPath)) docPath = assetDocPath;
        }
        if (!docPath.is_absolute()) {
            std::filesystem::path candidate = std::filesystem::absolute(docPath);
            if (std::filesystem::exists(candidate)) docPath = candidate;
        }
        std::filesystem::path relative = docPath.parent_path() / sourcePath;
        if (std::filesystem::exists(relative)) return std::filesystem::absolute(relative);
    }

    if (std::filesystem::exists(sourcePath)) return std::filesystem::absolute(sourcePath);

    std::filesystem::path assetCandidate(assetPath(sourcePath.generic_string()));
    if (std::filesystem::exists(assetCandidate)) return assetCandidate;
    return sourcePath;
}

ElementHandle* elementHandle(JSContext* ctx, JSValueConst value, JSClassID classId) {
    return static_cast<ElementHandle*>(JS_GetOpaque2(ctx, value, classId));
}

Rml::Element* liveElementFromJs(JSContext* ctx, JSValueConst value, JSClassID classId) {
    ElementHandle* handle = elementHandle(ctx, value, classId);
    if (!handle || !handle->canvas) return nullptr;
    return handle->canvas->isLiveElement(handle->element, handle->generation) ? handle->element : nullptr;
}

void jsElementFinalizer(JSRuntime*, JSValueConst value) {
    delete static_cast<ElementHandle*>(JS_GetOpaque(value, gElementClassId));
}

void jsClassListFinalizer(JSRuntime*, JSValueConst value) {
    delete static_cast<ElementHandle*>(JS_GetOpaque(value, gClassListClassId));
}

void jsStyleFinalizer(JSRuntime*, JSValueConst value) {
    delete static_cast<ElementHandle*>(JS_GetOpaque(value, gStyleClassId));
}

JSValue makeElementObject(JSContext* ctx, WebCanvasNode* canvas, Rml::Element* element, JSClassID classId) {
    if (!canvas || !element) return JS_NULL;
    JSValue obj = JS_NewObjectClass(ctx, classId);
    if (JS_IsException(obj)) return obj;
    auto* handle = new ElementHandle{canvas, element, canvas->documentGeneration()};
    JS_SetOpaque(obj, handle);
    return obj;
}

JSValue makeElementArray(JSContext* ctx, WebCanvasNode* canvas, const std::vector<Rml::Element*>& elements) {
    JSValue array = JS_NewArray(ctx);
    if (JS_IsException(array)) return array;
    uint32_t index = 0;
    for (Rml::Element* element : elements) {
        JS_SetPropertyUint32(ctx, array, index++, makeElementObject(ctx, canvas, element, gElementClassId));
    }
    return array;
}

float hotReloadPhase(const std::string& key) {
    if (key.empty()) return 0.0f;
    size_t bucket = std::hash<std::string>{}(key) % 1000u;
    return (static_cast<float>(bucket) / 1000.0f) * kHotReloadCheckIntervalSeconds;
}

class RmlDependencyCapture {
public:
    explicit RmlDependencyCapture(std::vector<std::string>& dependencies) {
        RmlUiRuntime::beginFileDependencyCapture(dependencies);
    }

    ~RmlDependencyCapture() {
        RmlUiRuntime::endFileDependencyCapture();
    }

    RmlDependencyCapture(const RmlDependencyCapture&) = delete;
    RmlDependencyCapture& operator=(const RmlDependencyCapture&) = delete;
};

WebCanvasNode* webCanvasFromJs(JSContext* ctx) {
    return static_cast<WebCanvasNode*>(JS_GetContextOpaque(ctx));
}

std::string escapeRmlText(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        case '\'': escaped += "&apos;"; break;
        default: escaped += c; break;
        }
    }
    return escaped;
}

JSValue jsDocumentSetText(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* canvas = webCanvasFromJs(ctx);
    if (!canvas || argc < 2) return JS_NewBool(ctx, false);

    const char* id = JS_ToCString(ctx, argv[0]);
    const char* text = JS_ToCString(ctx, argv[1]);
    bool ok = id && text && canvas->setElementText(id, text);
    JS_FreeCString(ctx, id);
    JS_FreeCString(ctx, text);
    return JS_NewBool(ctx, ok);
}

JSValue jsDocumentSetHTML(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* canvas = webCanvasFromJs(ctx);
    if (!canvas || argc < 2) return JS_NewBool(ctx, false);

    const char* id = JS_ToCString(ctx, argv[0]);
    const char* html = JS_ToCString(ctx, argv[1]);
    bool ok = id && html && canvas->setElementRml(id, html);
    JS_FreeCString(ctx, id);
    JS_FreeCString(ctx, html);
    return JS_NewBool(ctx, ok);
}

JSValue jsDocumentReload(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (auto* canvas = webCanvasFromJs(ctx)) {
        canvas->reload();
    }
    return JS_UNDEFINED;
}

JSValue jsDocumentGetElementById(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* canvas = webCanvasFromJs(ctx);
    if (!canvas || argc < 1) return JS_NULL;
    Rml::Element* element = canvas->findElementById(jsToString(ctx, argv[0]));
    return makeElementObject(ctx, canvas, element, gElementClassId);
}

JSValue jsDocumentQuerySelector(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* canvas = webCanvasFromJs(ctx);
    if (!canvas || argc < 1) return JS_NULL;
    Rml::Element* element = canvas->querySelector(jsToString(ctx, argv[0]));
    return makeElementObject(ctx, canvas, element, gElementClassId);
}

JSValue jsDocumentQuerySelectorAll(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* canvas = webCanvasFromJs(ctx);
    if (!canvas || argc < 1) return JS_NewArray(ctx);
    std::vector<Rml::Element*> elements;
    canvas->querySelectorAll(jsToString(ctx, argv[0]), elements);
    return makeElementArray(ctx, canvas, elements);
}

JSValue jsDocumentGetBody(JSContext* ctx, JSValueConst) {
    auto* canvas = webCanvasFromJs(ctx);
    if (!canvas) return JS_NULL;
    return makeElementObject(ctx, canvas, canvas->querySelector("body"), gElementClassId);
}

JSValue jsDocumentGetDocumentElement(JSContext* ctx, JSValueConst) {
    auto* canvas = webCanvasFromJs(ctx);
    if (!canvas) return JS_NULL;
    return makeElementObject(ctx, canvas, canvas->querySelector("rml"), gElementClassId);
}

JSValue jsElementQuerySelector(JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv) {
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gElementClassId);
    if (!element || argc < 1) return JS_NULL;
    ElementHandle* handle = elementHandle(ctx, thisValue, gElementClassId);
    return makeElementObject(ctx, handle ? handle->canvas : nullptr,
                             element->QuerySelector(jsToString(ctx, argv[0])), gElementClassId);
}

JSValue jsElementQuerySelectorAll(JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv) {
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gElementClassId);
    ElementHandle* handle = elementHandle(ctx, thisValue, gElementClassId);
    if (!element || !handle || !handle->canvas || argc < 1) return JS_NewArray(ctx);
    Rml::ElementList list;
    element->QuerySelectorAll(list, jsToString(ctx, argv[0]));
    std::vector<Rml::Element*> elements(list.begin(), list.end());
    return makeElementArray(ctx, handle->canvas, elements);
}

JSValue jsElementGetTextContent(JSContext* ctx, JSValueConst thisValue) {
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gElementClassId);
    return element ? JS_NewString(ctx, element->GetInnerRML().c_str()) : JS_UNDEFINED;
}

JSValue jsElementSetTextContent(JSContext* ctx, JSValueConst thisValue, JSValueConst value) {
    ElementHandle* handle = elementHandle(ctx, thisValue, gElementClassId);
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gElementClassId);
    if (!element || !handle || !handle->canvas) return JS_UNDEFINED;
    element->SetInnerRML(escapeRmlText(jsToString(ctx, value)));
    handle->canvas->notifyJsMutation();
    return JS_UNDEFINED;
}

JSValue jsElementGetInnerHTML(JSContext* ctx, JSValueConst thisValue) {
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gElementClassId);
    return element ? JS_NewString(ctx, element->GetInnerRML().c_str()) : JS_UNDEFINED;
}

JSValue jsElementSetInnerHTML(JSContext* ctx, JSValueConst thisValue, JSValueConst value) {
    ElementHandle* handle = elementHandle(ctx, thisValue, gElementClassId);
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gElementClassId);
    if (!element || !handle || !handle->canvas) return JS_UNDEFINED;
    element->SetInnerRML(jsToString(ctx, value));
    handle->canvas->notifyJsMutation();
    return JS_UNDEFINED;
}

JSValue jsElementGetId(JSContext* ctx, JSValueConst thisValue) {
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gElementClassId);
    return element ? JS_NewString(ctx, element->GetId().c_str()) : JS_UNDEFINED;
}

JSValue makeDomRect(JSContext* ctx, float left, float top, float width, float height) {
    JSValue rect = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rect, "x", JS_NewFloat64(ctx, left));
    JS_SetPropertyStr(ctx, rect, "y", JS_NewFloat64(ctx, top));
    JS_SetPropertyStr(ctx, rect, "left", JS_NewFloat64(ctx, left));
    JS_SetPropertyStr(ctx, rect, "top", JS_NewFloat64(ctx, top));
    JS_SetPropertyStr(ctx, rect, "width", JS_NewFloat64(ctx, width));
    JS_SetPropertyStr(ctx, rect, "height", JS_NewFloat64(ctx, height));
    JS_SetPropertyStr(ctx, rect, "right", JS_NewFloat64(ctx, left + width));
    JS_SetPropertyStr(ctx, rect, "bottom", JS_NewFloat64(ctx, top + height));
    return rect;
}

JSValue jsElementGetBoundingClientRect(JSContext* ctx, JSValueConst thisValue, int, JSValueConst*) {
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gElementClassId);
    if (!element) return makeDomRect(ctx, 0.0f, 0.0f, 0.0f, 0.0f);
    return makeDomRect(ctx, element->GetAbsoluteLeft(), element->GetAbsoluteTop(),
                       element->GetOffsetWidth(), element->GetOffsetHeight());
}

JSValue jsElementGetOffsetLeft(JSContext* ctx, JSValueConst thisValue) {
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gElementClassId);
    return JS_NewFloat64(ctx, element ? element->GetOffsetLeft() : 0.0f);
}

JSValue jsElementGetOffsetTop(JSContext* ctx, JSValueConst thisValue) {
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gElementClassId);
    return JS_NewFloat64(ctx, element ? element->GetOffsetTop() : 0.0f);
}

JSValue jsElementGetOffsetWidth(JSContext* ctx, JSValueConst thisValue) {
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gElementClassId);
    return JS_NewFloat64(ctx, element ? element->GetOffsetWidth() : 0.0f);
}

JSValue jsElementGetOffsetHeight(JSContext* ctx, JSValueConst thisValue) {
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gElementClassId);
    return JS_NewFloat64(ctx, element ? element->GetOffsetHeight() : 0.0f);
}

JSValue jsElementGetClientWidth(JSContext* ctx, JSValueConst thisValue) {
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gElementClassId);
    return JS_NewFloat64(ctx, element ? element->GetClientWidth() : 0.0f);
}

JSValue jsElementGetClientHeight(JSContext* ctx, JSValueConst thisValue) {
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gElementClassId);
    return JS_NewFloat64(ctx, element ? element->GetClientHeight() : 0.0f);
}

JSValue jsElementAddEventListener(JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv) {
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gElementClassId);
    ElementHandle* handle = elementHandle(ctx, thisValue, gElementClassId);
    if (!element || !handle || !handle->canvas || argc < 2 || !JS_IsFunction(ctx, argv[1])) {
        return JS_UNDEFINED;
    }
    handle->canvas->addJsEventListener(element, jsToString(ctx, argv[0]), ctx, argv[1]);
    return JS_UNDEFINED;
}

JSValue jsElementRemoveEventListener(JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv) {
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gElementClassId);
    ElementHandle* handle = elementHandle(ctx, thisValue, gElementClassId);
    if (!element || !handle || !handle->canvas || argc < 2 || !JS_IsFunction(ctx, argv[1])) {
        return JS_UNDEFINED;
    }
    handle->canvas->removeJsEventListener(element, jsToString(ctx, argv[0]), ctx, argv[1]);
    return JS_UNDEFINED;
}

JSValue jsElementClick(JSContext* ctx, JSValueConst thisValue, int, JSValueConst*) {
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gElementClassId);
    ElementHandle* handle = elementHandle(ctx, thisValue, gElementClassId);
    if (element && handle && handle->canvas) {
        element->Click();
        handle->canvas->notifyJsMutation();
    }
    return JS_UNDEFINED;
}

JSValue jsElementFocus(JSContext* ctx, JSValueConst thisValue, int, JSValueConst*) {
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gElementClassId);
    ElementHandle* handle = elementHandle(ctx, thisValue, gElementClassId);
    if (element && handle && handle->canvas) {
        element->Focus(true);
        handle->canvas->notifyJsMutation();
    }
    return JS_UNDEFINED;
}

JSValue jsElementBlur(JSContext* ctx, JSValueConst thisValue, int, JSValueConst*) {
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gElementClassId);
    ElementHandle* handle = elementHandle(ctx, thisValue, gElementClassId);
    if (element && handle && handle->canvas) {
        element->Blur();
        handle->canvas->notifyJsMutation();
    }
    return JS_UNDEFINED;
}

JSValue jsElementGetClassList(JSContext* ctx, JSValueConst thisValue) {
    ElementHandle* handle = elementHandle(ctx, thisValue, gElementClassId);
    if (!handle || !handle->canvas || !handle->canvas->isLiveElement(handle->element, handle->generation)) return JS_NULL;
    return makeElementObject(ctx, handle->canvas, handle->element, gClassListClassId);
}

JSValue jsElementGetStyle(JSContext* ctx, JSValueConst thisValue) {
    ElementHandle* handle = elementHandle(ctx, thisValue, gElementClassId);
    if (!handle || !handle->canvas || !handle->canvas->isLiveElement(handle->element, handle->generation)) return JS_NULL;
    return makeElementObject(ctx, handle->canvas, handle->element, gStyleClassId);
}

JSValue jsClassListAdd(JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv) {
    ElementHandle* handle = elementHandle(ctx, thisValue, gClassListClassId);
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gClassListClassId);
    if (!element || !handle || !handle->canvas) return JS_UNDEFINED;
    for (int i = 0; i < argc; ++i) element->SetClass(jsToString(ctx, argv[i]), true);
    handle->canvas->notifyJsMutation();
    return JS_UNDEFINED;
}

JSValue jsClassListRemove(JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv) {
    ElementHandle* handle = elementHandle(ctx, thisValue, gClassListClassId);
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gClassListClassId);
    if (!element || !handle || !handle->canvas) return JS_UNDEFINED;
    for (int i = 0; i < argc; ++i) element->SetClass(jsToString(ctx, argv[i]), false);
    handle->canvas->notifyJsMutation();
    return JS_UNDEFINED;
}

JSValue jsClassListToggle(JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv) {
    ElementHandle* handle = elementHandle(ctx, thisValue, gClassListClassId);
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gClassListClassId);
    if (!element || !handle || !handle->canvas || argc < 1) return JS_NewBool(ctx, false);
    std::string cls = jsToString(ctx, argv[0]);
    bool active = !element->IsClassSet(cls);
    if (argc >= 2) active = JS_ToBool(ctx, argv[1]) != 0;
    element->SetClass(cls, active);
    handle->canvas->notifyJsMutation();
    return JS_NewBool(ctx, active);
}

JSValue jsClassListContains(JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv) {
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gClassListClassId);
    if (!element || argc < 1) return JS_NewBool(ctx, false);
    return JS_NewBool(ctx, element->IsClassSet(jsToString(ctx, argv[0])));
}

JSValue jsStyleSetProperty(JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv) {
    ElementHandle* handle = elementHandle(ctx, thisValue, gStyleClassId);
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gStyleClassId);
    if (!element || !handle || !handle->canvas || argc < 2) return JS_NewBool(ctx, false);
    bool ok = element->SetProperty(jsToString(ctx, argv[0]), jsToString(ctx, argv[1]));
    handle->canvas->notifyJsMutation();
    return JS_NewBool(ctx, ok);
}

JSValue jsStyleRemoveProperty(JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv) {
    ElementHandle* handle = elementHandle(ctx, thisValue, gStyleClassId);
    Rml::Element* element = liveElementFromJs(ctx, thisValue, gStyleClassId);
    if (!element || !handle || !handle->canvas || argc < 1) return JS_UNDEFINED;
    element->RemoveProperty(jsToString(ctx, argv[0]));
    handle->canvas->notifyJsMutation();
    return JS_UNDEFINED;
}

JSValue jsTreeChangeScene(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* canvas = webCanvasFromJs(ctx);
    if (!canvas || !canvas->tree() || argc < 1) return JS_NewBool(ctx, false);
    canvas->tree()->changeScene(jsToString(ctx, argv[0]));
    return JS_NewBool(ctx, true);
}

JSValue jsTreeReloadScene(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* canvas = webCanvasFromJs(ctx);
    if (canvas && canvas->tree()) canvas->tree()->reloadScene();
    return JS_UNDEFINED;
}

JSValue jsTreeQuit(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* canvas = webCanvasFromJs(ctx);
    if (canvas && canvas->tree()) canvas->tree()->quit();
    return JS_UNDEFINED;
}

JSValue jsTreeSetPaused(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* canvas = webCanvasFromJs(ctx);
    if (canvas && canvas->tree() && argc >= 1) canvas->tree()->setPaused(JS_ToBool(ctx, argv[0]) != 0);
    return JS_UNDEFINED;
}

JSValue jsTreeGetPaused(JSContext* ctx, JSValueConst) {
    auto* canvas = webCanvasFromJs(ctx);
    return JS_NewBool(ctx, canvas && canvas->tree() && canvas->tree()->paused());
}

class JsRmlEventListener final : public Rml::EventListener {
public:
    JsRmlEventListener(WebCanvasNode& canvas, JSContext* ctx, JSValueConst callback)
        : canvas_(canvas), ctx_(ctx), callback_(JS_DupValue(ctx, callback)) {}

    ~JsRmlEventListener() override {
        JS_FreeValue(ctx_, callback_);
    }

    void ProcessEvent(Rml::Event& event) override {
        JSValue eventObject = JS_NewObject(ctx_);
        JS_SetPropertyStr(ctx_, eventObject, "type", JS_NewString(ctx_, event.GetType().c_str()));
        JS_SetPropertyStr(ctx_, eventObject, "target",
            makeElementObject(ctx_, &canvas_, event.GetTargetElement(), gElementClassId));
        JS_SetPropertyStr(ctx_, eventObject, "currentTarget",
            makeElementObject(ctx_, &canvas_, event.GetCurrentElement(), gElementClassId));
        JSValue result = JS_Call(ctx_, callback_, JS_UNDEFINED, 1, &eventObject);
        if (JS_IsException(result)) {
            JSValue exc = JS_GetException(ctx_);
            const char* message = JS_ToCString(ctx_, exc);
            Log::error("[WebCanvas JS] event listener: ", message ? message : "unknown exception");
            JS_FreeCString(ctx_, message);
            JS_FreeValue(ctx_, exc);
        }
        JS_FreeValue(ctx_, result);
        JS_FreeValue(ctx_, eventObject);
        canvas_.notifyJsMutation();
    }

    JSContext* context() const { return ctx_; }
    JSValueConst callback() const { return callback_; }

private:
    WebCanvasNode& canvas_;
    JSContext* ctx_ = nullptr;
    JSValue callback_ = JS_UNDEFINED;
};

void installJsClass(JSContext* ctx, JSClassID& id, const char* name, JSClassFinalizer* finalizer) {
    JSRuntime* runtime = JS_GetRuntime(ctx);
    if (id == 0) JS_NewClassID(runtime, &id);
    JSClassDef def{};
    def.class_name = name;
    def.finalizer = finalizer;
    JS_NewClass(runtime, id, &def);
}

JSValue makeGetter(JSContext* ctx, JSValue (*getter)(JSContext*, JSValueConst), const char* name) {
    JSCFunctionType fn{};
    fn.getter = getter;
    return JS_NewCFunction2(ctx, fn.generic, name, 0, JS_CFUNC_getter, 0);
}

JSValue makeSetter(JSContext* ctx, JSValue (*setter)(JSContext*, JSValueConst, JSValueConst), const char* name) {
    JSCFunctionType fn{};
    fn.setter = setter;
    return JS_NewCFunction2(ctx, fn.generic, name, 1, JS_CFUNC_setter, 0);
}

void defineGetSet(JSContext* ctx, JSValueConst object, const char* name, JSValue getter, JSValue setter) {
    JSAtom atom = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, object, atom, getter, setter, JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, atom);
}

} // namespace

struct WebCanvasNode::JsEventListenerRecord {
    Rml::Element* element = nullptr;
    std::string event;
    std::unique_ptr<JsRmlEventListener> listener;
};

WebCanvasNode::WebCanvasNode() = default;

WebCanvasNode::~WebCanvasNode() {
    if (rmlContext_) {
        rmlContext_->UnloadAllDocuments();
        rmlContext_->Update();
        clearJsEventListeners();
        Rml::RemoveContext(rmlContextName_);
    }
}

void WebCanvasNode::init(VulkanDevice& device, uint32_t width, uint32_t height, Mode mode) {
    width_ = width;
    height_ = height;
    mode_ = mode;
    device_ = &device;
    createPlaceholderTexture();
    ensureRmlContext();
    ensureJsContext();
    loadDocumentFromState();
}

void WebCanvasNode::loadHTML(const std::string& html) {
    url_.clear();
    html_ = html;
    documentWatchers_.clear();
    ensureRmlContext();
    loadDocumentFromState();
    markUiDirty();
    ensureJsContext();
}

void WebCanvasNode::loadURL(const std::string& url) {
    url_ = url;
    hotReloadTimer_ = hotReloadPhase(url_);
    html_.clear();
    documentWatchers_.clear();
    ensureRmlContext();
    loadDocumentFromState();
    markUiDirty();
    ensureJsContext();
}

void WebCanvasNode::setUrl(const std::string& url) {
    loadURL(url);
}

void WebCanvasNode::reload() {
    if (!url_.empty()) {
        loadURL(url_);
    } else if (!html_.empty()) {
        loadHTML(html_);
    }
}

void WebCanvasNode::setMode(Mode mode) {
    if (mode_ == mode) return;
    mode_ = mode;
    markUiDirty();
}

void WebCanvasNode::resize(uint32_t width, uint32_t height) {
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    if (width_ == width && height_ == height) return;
    width_ = width;
    height_ = height;
    createPlaceholderTexture();
    ensureRmlContext();
    markUiDirty();
}

glm::vec2 WebCanvasNode::screenPosition() const {
    return {transform_.position.x, transform_.position.y};
}

glm::vec2 WebCanvasNode::screenSize() const {
    return {
        static_cast<float>(width_) * std::max(transform_.scale.x, 0.0f),
        static_cast<float>(height_) * std::max(transform_.scale.y, 0.0f)
    };
}

bool WebCanvasNode::screenContains(glm::vec2 point) const {
    glm::vec2 pos = screenPosition();
    glm::vec2 size = screenSize();
    return point.x >= pos.x && point.x <= pos.x + size.x &&
           point.y >= pos.y && point.y <= pos.y + size.y;
}

glm::vec2 WebCanvasNode::screenToLocal(glm::vec2 point) const {
    glm::vec2 size = screenSize();
    glm::vec2 pos = screenPosition();
    float sx = size.x > 0.0f ? static_cast<float>(width_) / size.x : 1.0f;
    float sy = size.y > 0.0f ? static_cast<float>(height_) / size.y : 1.0f;
    return {(point.x - pos.x) * sx, (point.y - pos.y) * sy};
}

bool WebCanvasNode::hitTest(glm::vec2 local) const {
    if (!rmlContext_ || !interactive_ || !document_) return false;
    if (local.x < 0.0f || local.y < 0.0f ||
        local.x > static_cast<float>(width_) || local.y > static_cast<float>(height_)) {
        return false;
    }

    Rml::Element* element = rmlContext_->GetElementAtPoint({local.x, local.y});
    while (element && element != document_) {
        if (element->IsClassSet("ui-hit")) return true;
        const std::string tag = element->GetTagName();
        if (tag == "button" || tag == "input" || tag == "select" || tag == "textarea") {
            return true;
        }
        element = element->GetParentNode();
    }
    return false;
}

float WebCanvasNode::worldHeight() const {
    if (width_ == 0) return worldWidth_;
    return worldWidth_ * (static_cast<float>(height_) / static_cast<float>(width_));
}

bool WebCanvasNode::raycast(glm::vec3 origin, glm::vec3 direction, glm::vec2& local, float& distance) const {
    if (mode_ != Mode::WorldSpace) return false;

    WorldPanelHit hit;
    if (!raycastWorldPanel(worldTransform(), worldWidth_, worldHeight(), width_, height_,
                           origin, direction, hit)) {
        return false;
    }
    local = hit.local;
    distance = hit.distance;
    return true;
}

void WebCanvasNode::executeJS(const std::string& script) {
    ensureJsContext();
    if (jsContext_ && jsContext_->eval(script, url_.empty() ? "<web-canvas>" : url_)) {
        markUiDirty();
    }
}

bool WebCanvasNode::setElementText(const std::string& id, const std::string& text) {
    return setElementRml(id, escapeRmlText(text));
}

bool WebCanvasNode::setElementRml(const std::string& id, const std::string& rml) {
    if (!document_) return false;
    Rml::Element* element = document_->GetElementById(id);
    if (!element) return false;
    element->SetInnerRML(rml);
    if (rmlContext_) rmlContext_->Update();
    markUiDirty();
    return true;
}

bool WebCanvasNode::fireMouseEvent(MouseEvent type, int x, int y, MouseButton button, int modifiers) {
    if (!rmlContext_ || !interactive_) return false;

    bool handled = rmlContext_->ProcessMouseMove(x, y, modifiers);

    int rmlButton = 0;
    if (button == MouseButton::Right) rmlButton = 1;
    if (button == MouseButton::Middle) rmlButton = 2;
    if (button == MouseButton::None) return handled;
    if (type == MouseEvent::Down) {
        handled = rmlContext_->ProcessMouseButtonDown(rmlButton, modifiers) || handled;
    } else if (type == MouseEvent::Up) {
        handled = rmlContext_->ProcessMouseButtonUp(rmlButton, modifiers) || handled;
        if (button == MouseButton::Left) {
            if (Rml::Element* element = rmlContext_->GetElementAtPoint({static_cast<float>(x), static_cast<float>(y)})) {
                element->Click();
                handled = true;
            }
        }
    }
    markUiDirty();
    return handled;
}

bool WebCanvasNode::fireScrollEvent(float deltaX, float deltaY, int modifiers) {
    if (!rmlContext_ || !interactive_) return false;
    bool handled = rmlContext_->ProcessMouseWheel(Rml::Vector2f(deltaX, deltaY), modifiers);
    markUiDirty();
    return handled;
}

bool WebCanvasNode::fireKeyEvent(bool down, Rml::Input::KeyIdentifier key, int modifiers) {
    if (!rmlContext_ || !interactive_ || key == Rml::Input::KI_UNKNOWN) return false;
    bool handled = down ? rmlContext_->ProcessKeyDown(key, modifiers)
                        : rmlContext_->ProcessKeyUp(key, modifiers);
    markUiDirty();
    return handled;
}

bool WebCanvasNode::fireTextInput(uint32_t codepoint) {
    if (!rmlContext_ || !interactive_ || codepoint == 0) return false;
    bool handled = rmlContext_->ProcessTextInput(static_cast<Rml::Character>(codepoint));
    markUiDirty();
    return handled;
}

bool WebCanvasNode::fireTouchEvent(uint64_t id, glm::vec2 position, TouchEvent type, int modifiers) {
    if (!rmlContext_ || !interactive_) return false;
    Rml::TouchList touches;
    touches.push_back({static_cast<Rml::TouchId>(id), Rml::Vector2f(position.x, position.y)});

    bool handled = false;
    switch (type) {
    case TouchEvent::Start:
        handled = rmlContext_->ProcessTouchStart(touches, modifiers);
        break;
    case TouchEvent::Move:
        handled = rmlContext_->ProcessTouchMove(touches, modifiers);
        break;
    case TouchEvent::End:
        handled = rmlContext_->ProcessTouchEnd(touches, modifiers);
        break;
    case TouchEvent::Cancel:
        handled = rmlContext_->ProcessTouchCancel(touches);
        break;
    }
    markUiDirty();
    return handled;
}

void WebCanvasNode::updateTextureIfNeededAsync(rhi::vulkan::CommandEncoder& encoder) {
    SAIDA_PROFILE_FUNCTION();
    checkHotReload();
    if (!device_ || !texture_ || !rmlContext_ || !uiDirty_) return;

    {
        SAIDA_PROFILE_SCOPE("WebCanvas/RmlUpdate");
        rmlContext_->Update();
    }
    RmlUiRenderInterface* renderer = RmlUiRuntime::renderer();
    if (!renderer) return;

    renderer->beginFrame(width_, height_);
    std::vector<std::string> renderDependencies;
    {
        SAIDA_PROFILE_SCOPE("WebCanvas/RmlRender");
        RmlDependencyCapture capture(renderDependencies);
        rmlContext_->Render();
    }
    renderer->endFrame();
    appendDocumentWatchers(renderDependencies);

    const std::vector<uint8_t>& pixels = renderer->pixels();
    if (pixels.empty()) return;

    if (!loggedRenderStats_) {
        size_t visiblePixels = 0;
        uint32_t minX = width_;
        uint32_t minY = height_;
        uint32_t maxX = 0;
        uint32_t maxY = 0;
        for (uint32_t y = 0; y < height_; ++y) {
            for (uint32_t x = 0; x < width_; ++x) {
                size_t i = (static_cast<size_t>(y) * width_ + x) * 4 + 3;
                if (i < pixels.size() && pixels[i] != 0) {
                    ++visiblePixels;
                    minX = std::min(minX, x);
                    minY = std::min(minY, y);
                    maxX = std::max(maxX, x);
                    maxY = std::max(maxY, y);
                }
            }
        }
        if (visiblePixels > 0) {
            Log::info("[WebCanvas] '", name(), "' rendered ", visiblePixels,
                      " visible pixel(s), bbox=", minX, ",", minY, " -> ", maxX, ",", maxY);
        } else {
            Log::warn("[WebCanvas] '", name(), "' rendered 0 visible pixels");
        }
        loggedRenderStats_ = true;
    }

    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(pixels.size());
    if (!stagingBuffer_ || stagingBuffer_->size() != byteCount) {
        SAIDA_PROFILE_COUNTER_ADD("WebCanvas/StagingRecreates", 1);
        stagingBuffer_ = std::make_unique<Buffer>(*device_, byteCount, rhi::BufferUsage::TransferSrc, MemoryUsage::HostVisible);
    }
    {
        SAIDA_PROFILE_SCOPE("WebCanvas/Upload");
        stagingBuffer_->write(pixels.data(), byteCount);
        texture_->updatePixelsAsync(encoder, *stagingBuffer_, width_, height_);
    }
    SAIDA_PROFILE_COUNTER_ADD("WebCanvas/Uploads", 1);
    SAIDA_PROFILE_COUNTER_ADD("WebCanvas/UploadBytes", static_cast<double>(byteCount));
    uiDirty_ = false;
}

void WebCanvasNode::serialize(nlohmann::json& j, ResourceManager& resources) const {
    Node::serialize(j, resources);
    j["width"] = width_;
    j["height"] = height_;
    j["mode"] = static_cast<int>(mode_);
    j["url"] = url_;
    j["html"] = html_;
    j["hotReload"] = hotReloadEnabled_;
    j["startupScripts"] = startupScripts_;
    j["worldWidth"] = worldWidth_;
    j["interactive"] = interactive_;
    j["renderOrder"] = renderOrder_;
}

void WebCanvasNode::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    Node::deserialize(j, resources);
    width_ = j.value("width", 800u);
    height_ = j.value("height", 600u);
    mode_ = static_cast<Mode>(j.value("mode", static_cast<int>(Mode::ScreenSpace)));
    url_ = j.value("url", "");
    hotReloadTimer_ = hotReloadPhase(url_);
    html_ = j.value("html", "");
    hotReloadEnabled_ = j.value("hotReload", true);
    worldWidth_ = j.value("worldWidth", 1.0f);
    interactive_ = j.value("interactive", true);
    renderOrder_ = j.value("renderOrder", 0);
    if (j.contains("startupScripts")) {
        startupScripts_ = j["startupScripts"].get<std::vector<std::string>>();
    }

    init(resources.device(), width_, height_, mode_);
}

void WebCanvasNode::ensureJsContext() {
    if (!jsContext_) {
        jsContext_ = JsRuntime::instance().createContext();
        jsContext_->setOpaque(this);
        installDocumentBindings();
    }
}

void WebCanvasNode::ensureRmlContext() {
    if (!RmlUiRuntime::ensureInitialized()) return;

    if (rmlContext_) {
        rmlContext_->SetDimensions({static_cast<int>(width_), static_cast<int>(height_)});
        markUiDirty();
        return;
    }

    std::ostringstream name;
    name << "WebCanvas_" << gNextWebCanvasId.fetch_add(1);
    rmlContextName_ = name.str();
    rmlContext_ = Rml::CreateContext(
        rmlContextName_,
        {static_cast<int>(width_), static_cast<int>(height_)},
        RmlUiRuntime::renderInterface());

    if (!rmlContext_) {
        Log::error("[WebCanvas] failed to create RmlUi context");
    } else {
        markUiDirty();
    }
}

void WebCanvasNode::markUiDirty() {
    uiDirty_ = true;
}

void WebCanvasNode::installDocumentBindings() {
    if (!jsContext_) return;

    JSContext* ctx = jsContext_->raw();
    installJsClass(ctx, gElementClassId, "NextElement", jsElementFinalizer);
    installJsClass(ctx, gClassListClassId, "NextClassList", jsClassListFinalizer);
    installJsClass(ctx, gStyleClassId, "NextStyle", jsStyleFinalizer);

    JSValue elementProto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, elementProto, "addEventListener", JS_NewCFunction(ctx, jsElementAddEventListener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, elementProto, "removeEventListener", JS_NewCFunction(ctx, jsElementRemoveEventListener, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, elementProto, "click", JS_NewCFunction(ctx, jsElementClick, "click", 0));
    JS_SetPropertyStr(ctx, elementProto, "focus", JS_NewCFunction(ctx, jsElementFocus, "focus", 0));
    JS_SetPropertyStr(ctx, elementProto, "blur", JS_NewCFunction(ctx, jsElementBlur, "blur", 0));
    JS_SetPropertyStr(ctx, elementProto, "querySelector", JS_NewCFunction(ctx, jsElementQuerySelector, "querySelector", 1));
    JS_SetPropertyStr(ctx, elementProto, "querySelectorAll", JS_NewCFunction(ctx, jsElementQuerySelectorAll, "querySelectorAll", 1));
    JS_SetPropertyStr(ctx, elementProto, "getBoundingClientRect", JS_NewCFunction(ctx, jsElementGetBoundingClientRect, "getBoundingClientRect", 0));
    defineGetSet(ctx, elementProto, "id",
        makeGetter(ctx, jsElementGetId, "get id"), JS_UNDEFINED);
    defineGetSet(ctx, elementProto, "offsetLeft",
        makeGetter(ctx, jsElementGetOffsetLeft, "get offsetLeft"), JS_UNDEFINED);
    defineGetSet(ctx, elementProto, "offsetTop",
        makeGetter(ctx, jsElementGetOffsetTop, "get offsetTop"), JS_UNDEFINED);
    defineGetSet(ctx, elementProto, "offsetWidth",
        makeGetter(ctx, jsElementGetOffsetWidth, "get offsetWidth"), JS_UNDEFINED);
    defineGetSet(ctx, elementProto, "offsetHeight",
        makeGetter(ctx, jsElementGetOffsetHeight, "get offsetHeight"), JS_UNDEFINED);
    defineGetSet(ctx, elementProto, "clientWidth",
        makeGetter(ctx, jsElementGetClientWidth, "get clientWidth"), JS_UNDEFINED);
    defineGetSet(ctx, elementProto, "clientHeight",
        makeGetter(ctx, jsElementGetClientHeight, "get clientHeight"), JS_UNDEFINED);
    defineGetSet(ctx, elementProto, "textContent",
        makeGetter(ctx, jsElementGetTextContent, "get textContent"),
        makeSetter(ctx, jsElementSetTextContent, "set textContent"));
    defineGetSet(ctx, elementProto, "innerHTML",
        makeGetter(ctx, jsElementGetInnerHTML, "get innerHTML"),
        makeSetter(ctx, jsElementSetInnerHTML, "set innerHTML"));
    defineGetSet(ctx, elementProto, "innerRML",
        makeGetter(ctx, jsElementGetInnerHTML, "get innerRML"),
        makeSetter(ctx, jsElementSetInnerHTML, "set innerRML"));
    defineGetSet(ctx, elementProto, "classList",
        makeGetter(ctx, jsElementGetClassList, "get classList"), JS_UNDEFINED);
    defineGetSet(ctx, elementProto, "style",
        makeGetter(ctx, jsElementGetStyle, "get style"), JS_UNDEFINED);
    JS_SetClassProto(ctx, gElementClassId, elementProto);

    JSValue classListProto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, classListProto, "add", JS_NewCFunction(ctx, jsClassListAdd, "add", 1));
    JS_SetPropertyStr(ctx, classListProto, "remove", JS_NewCFunction(ctx, jsClassListRemove, "remove", 1));
    JS_SetPropertyStr(ctx, classListProto, "toggle", JS_NewCFunction(ctx, jsClassListToggle, "toggle", 1));
    JS_SetPropertyStr(ctx, classListProto, "contains", JS_NewCFunction(ctx, jsClassListContains, "contains", 1));
    JS_SetClassProto(ctx, gClassListClassId, classListProto);

    JSValue styleProto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, styleProto, "setProperty", JS_NewCFunction(ctx, jsStyleSetProperty, "setProperty", 2));
    JS_SetPropertyStr(ctx, styleProto, "removeProperty", JS_NewCFunction(ctx, jsStyleRemoveProperty, "removeProperty", 1));
    JS_SetClassProto(ctx, gStyleClassId, styleProto);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue document = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, document, "setText", JS_NewCFunction(ctx, jsDocumentSetText, "setText", 2));
    JS_SetPropertyStr(ctx, document, "setHTML", JS_NewCFunction(ctx, jsDocumentSetHTML, "setHTML", 2));
    JS_SetPropertyStr(ctx, document, "reload", JS_NewCFunction(ctx, jsDocumentReload, "reload", 0));
    JS_SetPropertyStr(ctx, document, "getElementById", JS_NewCFunction(ctx, jsDocumentGetElementById, "getElementById", 1));
    JS_SetPropertyStr(ctx, document, "querySelector", JS_NewCFunction(ctx, jsDocumentQuerySelector, "querySelector", 1));
    JS_SetPropertyStr(ctx, document, "querySelectorAll", JS_NewCFunction(ctx, jsDocumentQuerySelectorAll, "querySelectorAll", 1));
    defineGetSet(ctx, document, "body",
        makeGetter(ctx, jsDocumentGetBody, "get body"), JS_UNDEFINED);
    defineGetSet(ctx, document, "documentElement",
        makeGetter(ctx, jsDocumentGetDocumentElement, "get documentElement"), JS_UNDEFINED);
    JS_SetPropertyStr(ctx, global, "document", document);

    JSValue tree = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, tree, "changeScene", JS_NewCFunction(ctx, jsTreeChangeScene, "changeScene", 1));
    JS_SetPropertyStr(ctx, tree, "reloadScene", JS_NewCFunction(ctx, jsTreeReloadScene, "reloadScene", 0));
    JS_SetPropertyStr(ctx, tree, "quit", JS_NewCFunction(ctx, jsTreeQuit, "quit", 0));
    JS_SetPropertyStr(ctx, tree, "setPaused", JS_NewCFunction(ctx, jsTreeSetPaused, "setPaused", 1));
    defineGetSet(ctx, tree, "paused",
        makeGetter(ctx, jsTreeGetPaused, "get paused"), JS_UNDEFINED);
    JS_SetPropertyStr(ctx, global, "tree", tree);
    JS_FreeValue(ctx, global);
}

bool WebCanvasNode::loadDocumentFromState(bool keepExistingOnFailure) {
    if (!rmlContext_) return false;

    Rml::ElementDocument* nextDocument = nullptr;
    std::vector<std::string> dependencies;

    {
        RmlDependencyCapture capture(dependencies);
        if (!url_.empty()) {
            nextDocument = rmlContext_->LoadDocument(url_);
        } else if (!html_.empty()) {
            nextDocument = rmlContext_->LoadDocumentFromMemory(html_, "<web-canvas>");
        }
    }

    if (!nextDocument) {
        if (!url_.empty() || !html_.empty()) {
            setLastError("failed to load document");
            Log::warn("[WebCanvas] failed to load document");
        }
        if (!keepExistingOnFailure && document_) {
            rmlContext_->UnloadDocument(document_);
            clearJsEventListeners();
            document_ = nullptr;
            rmlContext_->Update();
            documentWatchers_.clear();
            markUiDirty();
        }
        lastLoadOk_ = false;
        return false;
    }

    Rml::ElementDocument* previousDocument = document_;
    const size_t previousListenerCount = jsEventListeners_.size();
    document_ = nextDocument;
    ++documentGeneration_;
    nextDocument->Show();
    if (previousDocument) previousDocument->Hide();
    rmlContext_->Update();

    ensureJsContext();
    bool scriptsOk = runDocumentScripts(&dependencies);
    if (!scriptsOk && keepExistingOnFailure && previousDocument) {
        rmlContext_->UnloadDocument(nextDocument);
        while (jsEventListeners_.size() > previousListenerCount) jsEventListeners_.pop_back();
        document_ = previousDocument;
        previousDocument->Show();
        ++documentGeneration_;
        rmlContext_->Update();
        markUiDirty();
        lastLoadOk_ = false;
        if (lastError_.empty()) setLastError("document script failed");
        return false;
    }

    if (previousDocument) {
        rmlContext_->UnloadDocument(previousDocument);
        jsEventListeners_.erase(jsEventListeners_.begin(), jsEventListeners_.begin() + previousListenerCount);
    }
    rmlContext_->Update();
    updateDocumentWatchers(dependencies);
    markUiDirty();
    lastLoadOk_ = scriptsOk;
    if (scriptsOk) setLastError({});
    return true;
}

std::string WebCanvasNode::resolveUrlPath() const {
    if (url_.empty()) return {};

    std::filesystem::path candidate(pathFromFileUrl(url_));
    if (candidate.is_absolute() && std::filesystem::exists(candidate)) return candidate.string();
    if (std::filesystem::exists(candidate)) return std::filesystem::absolute(candidate).string();

    std::filesystem::path rootRelative(assetPath(candidate.string()));
    if (std::filesystem::exists(rootRelative)) return rootRelative.string();

    return {};
}

void WebCanvasNode::updateDocumentWatchers(const std::vector<std::string>& dependencies) {
    dependencyPaths_ = dependencies;
    std::string rootPath = resolveUrlPath();
    if (!rootPath.empty() && std::find(dependencyPaths_.begin(), dependencyPaths_.end(), rootPath) == dependencyPaths_.end()) {
        dependencyPaths_.push_back(std::move(rootPath));
    }
    rebuildDocumentWatchers();
}

void WebCanvasNode::appendDocumentWatchers(const std::vector<std::string>& dependencies) {
    bool changed = false;
    for (const auto& dependency : dependencies) {
        if (std::find(dependencyPaths_.begin(), dependencyPaths_.end(), dependency) == dependencyPaths_.end()) {
            dependencyPaths_.push_back(dependency);
            changed = true;
        }
    }
    if (changed) rebuildDocumentWatchers();
}

bool WebCanvasNode::isLiveElement(Rml::Element* element, uint64_t generation) const {
    return element && document_ && generation == documentGeneration_ &&
           element->GetOwnerDocument() == document_;
}

Rml::Element* WebCanvasNode::findElementById(const std::string& id) const {
    return document_ ? document_->GetElementById(id) : nullptr;
}

Rml::Element* WebCanvasNode::querySelector(const std::string& selector) const {
    return document_ ? document_->QuerySelector(selector) : nullptr;
}

void WebCanvasNode::querySelectorAll(const std::string& selector, std::vector<Rml::Element*>& elements) const {
    elements.clear();
    if (!document_) return;
    Rml::ElementList list;
    document_->QuerySelectorAll(list, selector);
    elements.assign(list.begin(), list.end());
}

void WebCanvasNode::addJsEventListener(Rml::Element* element, const std::string& event, JSContext* ctx, JSValueConst callback) {
    if (!element || !ctx || !JS_IsFunction(ctx, callback)) return;
    auto record = std::make_unique<JsEventListenerRecord>();
    record->element = element;
    record->event = event;
    record->listener = std::make_unique<JsRmlEventListener>(*this, ctx, callback);
    element->AddEventListener(event, record->listener.get());
    jsEventListeners_.push_back(std::move(record));
}

void WebCanvasNode::removeJsEventListener(Rml::Element* element, const std::string& event, JSContext* ctx, JSValueConst callback) {
    auto it = std::find_if(jsEventListeners_.begin(), jsEventListeners_.end(),
        [&](const std::unique_ptr<JsEventListenerRecord>& record) {
            return record->element == element && record->event == event &&
                   record->listener && record->listener->context() == ctx &&
                   JS_IsStrictEqual(ctx, record->listener->callback(), callback);
        });
    if (it == jsEventListeners_.end()) return;
    if (element && (*it)->listener) {
        element->RemoveEventListener(event, (*it)->listener.get());
    }
    jsEventListeners_.erase(it);
}

void WebCanvasNode::notifyJsMutation() {
    if (rmlContext_) rmlContext_->Update();
    markUiDirty();
}

void WebCanvasNode::rebuildDocumentWatchers() {
    documentWatchers_.clear();

    documentWatchers_.reserve(dependencyPaths_.size());
    for (const auto& path : dependencyPaths_) {
        WatchedFile watcher;
        if (watcher.watch(path)) {
            documentWatchers_.push_back(std::move(watcher));
        }
    }
}

void WebCanvasNode::checkHotReload() {
    if (!hotReloadEnabled_ || url_.empty()) return;

    hotReloadTimer_ += Time::unscaledDelta();
    if (hotReloadTimer_ < kHotReloadCheckIntervalSeconds) return;
    hotReloadTimer_ = 0.0f;

    if (documentWatchers_.empty()) {
        std::vector<std::string> dependencies;
        updateDocumentWatchers(dependencies);
        return;
    }

    for (auto& watcher : documentWatchers_) {
        if (watcher.pollChanged()) {
            Log::info("[WebCanvas] hot reload ", watcher.path());
            if (!loadDocumentFromState(true)) {
                Log::warn("[WebCanvas] keeping previous document after reload failure");
            }
            return;
        }
    }
}

void WebCanvasNode::runStartupScripts() {
    for (const auto& script : startupScripts_) {
        executeJS(script);
    }
}

bool WebCanvasNode::runDocumentScripts(std::vector<std::string>* dependencies) {
    std::string source = html_;
    std::filesystem::path documentPath;
    if (!url_.empty()) {
        documentPath = resolveUrlPath();
        if (!documentPath.empty()) source = readTextFile(documentPath);
    }
    if (source.empty()) {
        runStartupScripts();
        return true;
    }

    ensureJsContext();
    if (!jsContext_) return false;

    bool ok = true;
    std::regex scriptTag(R"(<script\b([^>]*)>([\s\S]*?)</script>)", std::regex_constants::icase);
    std::regex srcAttr(R"(src\s*=\s*["']([^"']+)["'])", std::regex_constants::icase);
    auto begin = std::sregex_iterator(source.begin(), source.end(), scriptTag);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::string attrs = (*it)[1].str();
        std::string body = (*it)[2].str();
        std::smatch srcMatch;
        if (std::regex_search(attrs, srcMatch, srcAttr)) {
            std::string baseUrl = documentPath.empty() ? url_ : documentPath.generic_string();
            std::filesystem::path scriptPath = resolveDocumentRelative(srcMatch[1].str(), baseUrl);
            if (dependencies) dependencies->push_back(scriptPath.generic_string());
            std::string script = readTextFile(scriptPath);
            if (script.empty()) {
                setLastError("script not found: " + scriptPath.generic_string());
                Log::warn("[WebCanvas] script not found: ", scriptPath.generic_string());
                ok = false;
                continue;
            }
            ok = jsContext_->eval(script, scriptPath.generic_string()) && ok;
        } else if (!body.empty()) {
            ok = jsContext_->eval(body, url_.empty() ? "<web-canvas-script>" : url_) && ok;
        }
    }

    for (const auto& script : startupScripts_) {
        ok = jsContext_->eval(script, url_.empty() ? "<web-canvas-startup>" : url_) && ok;
    }
    if (!ok && lastError_.empty()) setLastError("script execution failed");
    return ok;
}

void WebCanvasNode::setLastError(std::string error) {
    lastError_ = std::move(error);
    lastLoadOk_ = lastError_.empty();
}

void WebCanvasNode::clearJsEventListeners() {
    jsEventListeners_.clear();
}

void WebCanvasNode::createPlaceholderTexture() {
    if (!device_ || width_ == 0 || height_ == 0) return;

    constexpr uint32_t kMinPlaceholderExtent = 2;
    uint32_t w = std::max(width_, kMinPlaceholderExtent);
    uint32_t h = std::max(height_, kMinPlaceholderExtent);

    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            size_t i = (static_cast<size_t>(y) * w + x) * 4;
            bool a = ((x / 32u) + (y / 32u)) % 2u == 0u;
            pixels[i + 0] = a ? 34 : 22;
            pixels[i + 1] = a ? 44 : 28;
            pixels[i + 2] = a ? 58 : 38;
            pixels[i + 3] = 255;
        }
    }

    texture_ = std::make_unique<Texture>(*device_, pixels.data(), w, h, rhi::Format::RGBA8Srgb, false);
    stagingBuffer_.reset();
    markUiDirty();
}

} // namespace saida
