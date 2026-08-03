#pragma once

#include <string>
#include <vector>

namespace saida {

// `saida_tool render-ui` — render a UI document without a GPU, a window or a
// human, and report what the layout engine actually computed.
//
// The CPU RmlUi backend already produces a full RGBA frame; this command is
// the supported entry point to it, so a golden UI image is as cheap to produce
// as a scene validation. `--layout-json` answers the question a picture cannot:
// *why* a document looks wrong (an element computing to `display: inline`, a
// box outside its context, a declaration RCSS refused).
//
// Returns the process exit code: 0 rendered, 1 the document was rejected,
// 2 bad invocation or I/O failure.
int runRenderUiCommand(const std::vector<std::string>& args);

} // namespace saida
