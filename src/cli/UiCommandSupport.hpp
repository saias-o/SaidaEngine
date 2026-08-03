#pragma once

#include <cstdint>
#include <string>

namespace Rml {
class Element;
}

namespace saida::ui_cli {

// Plumbing shared by the headless UI commands (`render-ui`, `validate-ui`).
// They must agree on their exit codes and on how they name things, or a caller
// cannot script the two together.

constexpr int kExitOk = 0;
constexpr int kExitInvalid = 1;  // the document was processed and rejected
constexpr int kExitUsage = 2;    // bad invocation or I/O failure

constexpr uint32_t kDefaultWidth = 1920;
constexpr uint32_t kDefaultHeight = 1080;
// A frame is width*height*4 bytes in memory before encoding; the cap keeps a
// mistyped --size from trying to allocate the machine's whole RAM.
constexpr uint32_t kMaxDimension = 16384;

// Parse "WxH". Returns false with `error` describing what was wrong.
bool parseSize(const std::string& text, uint32_t& width, uint32_t& height, std::string& error);

// Write `text` to `path`, or to stdout when `path` is "-". Creates the parent
// directory when needed.
bool writeText(const std::string& path, const std::string& text, std::string& error);

// The computed `display` of an element, as the CSS keyword.
const char* displayName(Rml::Element& element);

// A stable, readable path to an element inside its document, e.g.
// "body > div#menu-root > div.topbar". Used to point at an element in a report.
std::string elementPath(Rml::Element& element);

// How an element is named on its own: "div#id.class", "body", "#text".
std::string elementLabel(Rml::Element& element);

} // namespace saida::ui_cli
