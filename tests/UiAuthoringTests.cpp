#include "core/Paths.hpp"
#include "core/Time.hpp"
#include "ui/RmlUiRuntime.hpp"
#include "ui/RmlUiRenderInterface.hpp"
#include "ui/UiUpdateSchedule.hpp"
#include "ui/WebCss.hpp"
#include "ui/WebMarkup.hpp"
#include <RmlUi/Core.h>
#include <cassert>
#include <cmath>
#include <iostream>

using namespace saida;

int main() {
    UiUpdateSchedule schedule;
    assert(schedule.due(0, false));
    schedule.updated(1, 2);
    assert(!schedule.due(2.9, false));
    assert(schedule.due(3, false));
    assert(schedule.due(2, true));
    schedule.updated(3, std::numeric_limits<double>::infinity());
    assert(!schedule.due(100, false));
    assert(schedule.due(100, true));
    schedule.updated(100, 0);
    assert(schedule.due(100, false));

    assert(normalizeWebCss("rgba(1,2,3,0.5)") == "rgba(1,2,3,128)");
    assert(normalizeWebCss("RGBA(1, 2, 3, 1)") == "RGBA(1, 2, 3,255)");
    assert(normalizeWebCss("rgba(1,2,3,128)") == "rgba(1,2,3,128)");
    assert(normalizeWebCss("rgba(1,2,3,.25)") == "rgba(1,2,3,64)");
    assert(normalizeWebCss("/* rgba(1,2,3,.5) */ 'rgba(1,2,3,.5)'") == "/* rgba(1,2,3,.5) */ 'rgba(1,2,3,.5)'");
    assert(normalizeWebCss("rgba(1,2,3,)") == "rgba(1,2,3,)");
    assert(normalizeWebCss("rgba(1,2,3,nan)") == "rgba(1,2,3,nan)");
    assert(normalizeWebCss("div { border: 1px solid rgba(20,40,60,.5); }") == "div { border: 1px  rgba(20,40,60,128); }");
    assert(normalizeWebCss("div { border-bottom: 2px solid #fff; content: 'solid'; }") == "div { border-bottom: 2px  #fff; content: 'solid'; }");
    assert(normalizeWebStyleValue("border","2px solid rgba(1,2,3,.25)") == "2px  rgba(1,2,3,64)");
    assert(normalizeWebBorderValue("1px dashed red") == "1px dashed red");
    assert(normalizeWebBorderValue("1px /* solid */ solid red") == "1px /* solid */  red");
    assert(normalizeWebCss("rgba(1,2,3,50%)") == "rgba(1,2,3,50%)");
    assert(normalizeWebCss("button:hover { transition: opacity 180ms ease-out; }") ==
           "button:hover { transition: opacity 0.180000s cubic-out; }");
    assert(normalizeWebStyleValue("animation", "enter 800ms ease-in-out") == "enter 0.800000s cubic-in-out");
    assert(normalizeWebMarkup("<html><head><link rel='stylesheet' href='x.css'/></head><body><input><br></body></html>") ==
           "<rml><head><link rel='stylesheet' href='x.css' type=\"text/rcss\"/></head><body><input/><br/></body></rml>");
    const std::string script = "<script>const example = '<html><link rel=stylesheet></html>';</script>";
    assert(normalizeWebMarkup(script) == script);

    assert(RmlUiRuntime::ensureInitialized());
    auto* context = Rml::CreateContext("ui-authoring", {160, 100}, RmlUiRuntime::renderInterface());
    assert(context);
    auto* document = RmlUiRuntime::loadDocument(*context,
        assetPath("tests/fixtures/ui-project/ui/web-standard.html"));
    assert(document);
    document->Show(); context->Update();
    auto* nav = document->GetElementById("navigation");
    assert(nav && nav->GetBox().GetSize().x == 20);
    assert(!document->GetElementById("concealed")->IsVisible());
    assert(nav->SetProperty("border",normalizeWebStyleValue("border","2px solid rgba(20,40,60,.5)")));
    context->Update();
    assert(nav->GetProperty<float>("border-top-width")==2);
    assert(nav->GetProperty<Rml::Colourb>("border-top-color").alpha==128);
    nav->RemoveProperty("border");context->Update();
    auto* renderer = RmlUiRuntime::renderer();
    renderer->beginFrame(160, 100); context->Render(); renderer->endFrame();
    const auto& pixels = renderer->pixels();
    const size_t at = (15 * 160 + 5) * 4;
    assert(std::abs(int(pixels[at]) - 20) <= 1);
    assert(std::abs(int(pixels[at+3]) - 128) <= 1);
    const double uiBefore = Time::unscaledElapsed();
    const float gameBefore = Time::elapsed();
    Time::setScale(0);
    for (int i = 0; i < 30; ++i) { Time::advance(1.f/60.f); context->Update(); }
    assert(Time::elapsed() == gameBefore);
    assert(std::abs(Time::unscaledElapsed() - uiBefore - 0.5) < 1e-6);
    const float moved = document->GetElementById("animated")->GetAbsoluteOffset().x;
    assert(std::abs(moved - 10.f) < 0.1f);
    assert(context->GetNextUpdateDelay() == 0);
    for (int i = 0; i < 120; ++i) { Time::advance(1.f/60.f); context->Update(); }
    assert(std::abs(document->GetElementById("animated")->GetAbsoluteOffset().x - 40.f) < 0.1f);
    Time::setScale(1);

    // The captured command list must own its assets after the document unloads.
    renderer->beginCapture(160, 100); context->Render();
    auto captured = renderer->endCapture();
    assert(!captured.draws.empty());
    context->UnloadAllDocuments(); context->Update(); Rml::RemoveContext("ui-authoring");
    assert(!captured.draws.front().geometry->vertices.empty());
    RmlUiRuntime::shutdown();
    std::cout << "[ui-authoring] PASS: web defaults, CSS alpha, paused-game animation, scheduled wakeups, owned GPU draws\n";
}
