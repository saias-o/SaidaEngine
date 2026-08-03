#pragma once

#include <string>
#include <vector>

namespace saida {

// `saida_tool validate-ui` — report what a UI document gets wrong, before
// anyone has to look at it.
//
// RCSS reports a failed declaration as one warning in a log nobody reads at
// authoring time, and some mistakes it does not report at all: `rgba()` written
// with a 0-1 alpha parses as nothing and the property silently vanishes. An
// element carrying box properties while computing to `display: inline` is
// always a bug and is likewise silent. This command makes both mechanical.
//
// On the model of `validate-scene`: prints a JSON report, exit 0 when the
// document is clean, 1 when it is not, 2 on a bad invocation.
int runValidateUiCommand(const std::vector<std::string>& args);

} // namespace saida
