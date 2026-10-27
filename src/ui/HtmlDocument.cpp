#include "ui/HtmlDocument.hpp"
#include "ui/WebMarkup.hpp"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/XMLNodeHandler.h>
#include <RmlUi/Core/XMLParser.h>

namespace saida {
namespace {
constexpr const char* kHtmlDocumentInstancer = "saida-html-document";

class HtmlDocumentInstancer final : public Rml::ElementInstancer {
public:
    Rml::ElementPtr InstanceElement(Rml::Element*, const Rml::String&, const Rml::XMLAttributes&) override {
        return Rml::ElementPtr(new Rml::ElementDocument("html"));
    }
    void ReleaseElement(Rml::Element* element) override { delete element; }
};

class HtmlRootHandler final : public Rml::XMLNodeHandler {
public:
    Rml::Element* ElementStart(Rml::XMLParser* parser, const Rml::String&,
                              const Rml::XMLAttributes& attributes) override {
        auto* document = parser->GetParseFrame()->element->GetOwnerDocument();
        if (document && document->GetTagName() == "html") document->SetAttributes(attributes);
        parser->PushDefaultHandler();
        return document;
    }
    bool ElementEnd(Rml::XMLParser*, const Rml::String&) override { return true; }
    bool ElementData(Rml::XMLParser* parser, const Rml::String& text, Rml::XMLDataType) override {
        return Rml::Factory::InstanceElementText(parser->GetParseFrame()->element, text);
    }
};

// The context may load HTML and native RML documents side by side. Restore its
// document instancer before returning, including on a failed load.
class DocumentTagScope {
public:
    DocumentTagScope(Rml::Context& context, bool html) : context_(context), previous_(context.GetDocumentsBaseTag()) {
        if (html) context_.SetDocumentsBaseTag(kHtmlDocumentInstancer);
    }
    ~DocumentTagScope() { context_.SetDocumentsBaseTag(previous_); }
private:
    Rml::Context& context_;
    Rml::String previous_;
};
}

void installHtmlDocumentSupport() {
    static HtmlDocumentInstancer instancer;
    Rml::Factory::RegisterElementInstancer(kHtmlDocumentInstancer, &instancer);
    Rml::XMLParser::RegisterNodeHandler("html", Rml::MakeShared<HtmlRootHandler>());
}

Rml::ElementDocument* loadUiMarkup(Rml::Context& context, const std::string& markup,
                                  const std::string& sourceName) {
    bool html = false;
    const auto normalized = normalizeWebMarkup(markup, &html);
    DocumentTagScope documentTag(context, html);
    Rml::String sourceUrl = sourceName;
    // Match StreamFile's Windows drive escaping when using an in-memory stream.
    if (sourceUrl.size() > 1 && sourceUrl[1] == ':') sourceUrl[1] = '|';
    return context.LoadDocumentFromMemory(normalized, sourceUrl);
}
}
