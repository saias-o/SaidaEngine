#pragma once

#include "render/RenderFeature.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace saida {

// Registry of scene-pass render features, sorted by draw order.
class RenderFeatureRegistry {
public:
    using Factory = std::function<std::unique_ptr<ScenePassFeature>()>;

    static RenderFeatureRegistry& instance();

    // `order` is the draw order inside the HDR scene pass (lower draws first).
    void add(int order, Factory factory);

    // Fresh instances of every registered feature, sorted by order. Called once per
    // Renderer (the desktop and XR renderers each get their own independent set).
    std::vector<std::unique_ptr<ScenePassFeature>> build() const;

private:
    struct Entry { int order; Factory factory; };
    std::vector<Entry> entries_;
};

// Central registration avoids static-library dead stripping of feature factories.
void registerBuiltinRenderFeatures();

} // namespace saida
