#include "core/Paths.hpp"
#include "ui/RmlUiRuntime.hpp"
#include <RmlUi/Core.h>
#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace saida;

namespace {
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "[ui-box-sizing] FAIL: " << message << '\n';
        std::exit(1);
    }
}
void size(Rml::Element* element, float width, float height) {
    require(element != nullptr, "element exists");
    const auto actual = element->GetBox().GetSize(Rml::BoxArea::Border);
    if (std::abs(actual.x - width) > 0.01f || std::abs(actual.y - height) > 0.01f)
        std::cerr << element->GetId() << ": " << actual.x << 'x' << actual.y
                  << ", expected " << width << 'x' << height << '\n';
    require(std::abs(actual.x - width) < 0.01f && std::abs(actual.y - height) < 0.01f,
            "border box agrees with CSS sizing");
}
}

int main() {
    require(RmlUiRuntime::ensureInitialized(), "runtime initializes");
    auto* context = Rml::CreateContext("box-sizing", {800, 1800}, RmlUiRuntime::renderInterface());
    require(context != nullptr, "context created");
    std::vector<RmlUiDiagnostic> diagnostics;
    RmlUiRuntime::beginLogCapture(diagnostics);
    auto* doc = RmlUiRuntime::loadDocument(*context,
        assetPath("tests/fixtures/ui-project/ui/box-sizing.html"));
    require(doc != nullptr, "ordinary HTML and external CSS load");
    doc->Show(); context->Update();
    auto get = [&doc](const char* id) { return doc->GetElementById(id); };
    size(get("content"), 224, 104);
    for (const char* id : {"border", "inherited", "variable", "percentage", "flex-a", "flex-b",
                           "minimum", "maximum", "button", "select", "checkbox"})
        size(get(id), 200, 80);
    for (const char* id : {"initial", "unset", "input-text", "textarea", "barrier-child"})
        size(get(id), 224, 104);
    size(get("tiny"), 24, 24);
    require(get("tiny")->GetBox().GetSize(Rml::BoxArea::Content) == Rml::Vector2f(0, 0),
            "content cannot become negative");
    require(get("inherited")->GetLocalProperty("box-sizing")->ToString() == "inherit",
            "local CSS retains the authored inheritance keyword");
    require(get("inherited")->GetProperty<int>("box-sizing") == int(Rml::Style::BoxSizing::BorderBox),
            "resolved property agrees with computed layout");

    get("parent")->SetClass("content", true); context->Update();
    size(get("inherited"), 224, 104);
    size(get("variable"), 224, 104);
    get("parent")->SetClass("content", false); context->Update();
    size(get("inherited"), 200, 80);
    require(get("parent")->SetProperty("box-sizing", "content-box"), "parent inline style accepted");
    context->Update(); size(get("inherited"), 224, 104);
    get("parent")->RemoveProperty("box-sizing"); context->Update(); size(get("inherited"), 200, 80);
    require(get("parent")->SetProperty("--sizing", "content-box"), "custom property accepted");
    context->Update(); size(get("variable"), 224, 104);
    get("parent")->RemoveProperty("--sizing"); context->Update(); size(get("variable"), 200, 80);

    auto moved = get("chain")->RemoveChild(get("inherited"));
    get("other")->AppendChild(std::move(moved)); context->Update(); size(get("inherited"), 224, 104);
    moved = get("other")->RemoveChild(get("inherited"));
    get("chain")->AppendChild(std::move(moved)); context->Update(); size(get("inherited"), 200, 80);
    require(get("inherited")->SetProperty("box-sizing", "INITIAL"), "keywords are case insensitive");
    context->Update(); size(get("inherited"), 224, 104);
    get("inherited")->RemoveProperty("box-sizing"); context->Update(); size(get("inherited"), 200, 80);

    context->UnloadAllDocuments(); context->Update();
    doc = RmlUiRuntime::loadDocument(*context,
        assetPath("tests/fixtures/ui-project/ui/box-sizing-reset.html"));
    require(doc != nullptr, "standard html/universal/pseudo-element reset loads");
    doc->Show(); context->Update();
    require(doc->GetTagName() == "html" && doc->GetId() == "root", "html is the real document element");
    require(doc->GetAttribute<Rml::String>("lang", "") == "en", "HTML root attributes preserved");
    auto* body = doc->QuerySelector("body");
    require(body && body != doc && body->GetParentNode() == doc, "body is a distinct child of html");
    require(!doc->HasAttribute("data-body") && body->HasAttribute("data-body"), "body attributes stay on body");
    require(doc->Matches(":root"), ":root matches the document element");
    require(body->QuerySelector(":root") == nullptr, "element-scoped queries cannot return an ancestor");
    for (const char* id : {"reset-div", "reset-button", "reset-checkbox", "root-variable"}) size(get(id), 200, 80);
    size(get("escape"), 224, 104);
    size(body, 400, 600);
    require(doc->SetProperty("box-sizing", "content-box"), "reset parent changes");
    context->Update();
    size(get("reset-button"), 224, 104);
    size(get("reset-checkbox"), 224, 104);
    size(body, 410, 610);
    require(get("reset-button")->SetProperty("box-sizing", "initial"), "initial overrides UA");
    require(doc->SetProperty("box-sizing", "border-box"), "reset parent changes back");
    context->Update(); size(get("reset-button"), 224, 104);
    size(get("reset-div"), 200, 80);
    // Switching formats in one context must not leak the HTML document instancer.
    auto* native = RmlUiRuntime::loadDocumentFromMemory(*context,
        "<rml><head></head><body id='native-body'></body></rml>", "<native-after-html>");
    require(native && native->GetTagName() == "body", "native RML retains its document-as-body contract");
    RmlUiRuntime::endLogCapture();
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity != "info") std::cerr << diagnostic.message << '\n';
        require(diagnostic.severity == "info", "HTML/CSS parses without warnings");
    }
    require(!get("reset-button")->SetProperty("box-sizing", "padding-box"), "invalid values rejected");
    context->UnloadAllDocuments(); context->Update(); Rml::RemoveContext("box-sizing");
    RmlUiRuntime::shutdown();
    std::cout << "[ui-box-sizing] PASS (" << checks << " checks)\n";
}
