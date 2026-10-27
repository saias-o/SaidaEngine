#pragma once
#include <string>

namespace Rml { class Context; class ElementDocument; }

namespace saida {
void installHtmlDocumentSupport();
Rml::ElementDocument* loadUiMarkup(Rml::Context& context, const std::string& markup,
                                  const std::string& sourceName);
}
