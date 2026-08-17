#pragma once

#include <string>
#include <vector>

namespace saida {

// `saida_tool compare-png` — the second half of the visual net. A capture is
// only evidence once something can say whether it changed, and by how much and
// where: a bounding box around the difference is what separates "the tonemap
// moved" from "the FPS counter ticked over".
//
// Comparison is exact by default. A tolerance exists because a GPU is allowed
// to differ from a software rasterizer in the last bit of a channel, not so a
// failing image can be waved through: both knobs are reported in the result, so
// a permissive run cannot look like a strict one.
//
// Returns the process exit code: 0 the images match within the stated bounds,
// 1 they differ, 2 bad invocation or I/O failure.
int runComparePngCommand(const std::vector<std::string>& args);

} // namespace saida
