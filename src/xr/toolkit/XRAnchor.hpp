#pragma once

#include "scene/Node.hpp"

namespace saida {

// A node pinned to a real-world spatial anchor (AR). On ready it registers its
// pose with the XRAnchors backend; each frame an internal tracker writes the
// backend's located pose back into the node's transform, so children stick to the
// physical world. With no backend installed it keeps its authored pose (behaves
// like a normal node) — clean degradation before the OpenXR anchor extension is
// wired. Put content you want anchored under it.
class XRAnchor : public Node {
public:
    XRAnchor();

    bool persistent = false;  // hint for a persisting backend (saved across runs)

    const char* typeName() const override { return "XRAnchor"; }
    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;
};

} // namespace saida
