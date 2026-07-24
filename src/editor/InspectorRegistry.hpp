#pragma once

#include <functional>
#include <string>
#include <unordered_map>

namespace saida {

class Behaviour;
class EditorUI;

// Maps a behaviour type name to an editor-side ImGui drawer.
//
// This keeps inspector UI out of the gameplay layer: behaviours no longer carry
// their own ImGui code (no `onDrawInspector`), so the engine library does not
// depend on ImGui through gameplay behaviours. The built-in drawers register
// themselves the first time the registry is used.
class InspectorRegistry {
public:
    using Drawer = std::function<void(Behaviour&, EditorUI&)>;

    static InspectorRegistry& instance();

    void registerDrawer(std::string typeName, Drawer drawer);

    // Draws the inspector for `b` if a drawer is registered for its typeName().
    // Returns true if one ran (so the caller can show a fallback otherwise).
    bool draw(Behaviour& b, EditorUI& editor) const;

private:
    std::unordered_map<std::string, Drawer> drawers_;
};

} // namespace saida
