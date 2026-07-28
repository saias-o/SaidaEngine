#pragma once

#include "render/TriangleMetrics.hpp"

namespace saida {

class Camera;
class Scene;

// This collector belongs to saida_editor, not saida_engine. Standalone desktop
// and Web players therefore neither link it nor traverse their scenes for these
// diagnostics.
MeshTriangleMetrics collectProfilerTriangleMetrics(const Scene& scene,
                                                   const Camera& camera);

} // namespace saida
