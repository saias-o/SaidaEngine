#pragma once

#include "editor/EditorUI.hpp"

namespace saida {

class Project;
class ResourceManager;
class Scene;

// Animation panel: plays back an Animator's clips, non-destructive editing
// of ClipViews (.sclip), and applying graphs (.sgraph) with live parameter
// control. Operates on the selected node's Animator, falling back to the
// first one found (including the preview scene).
class Animator;

class AnimationPanel {
public:
    void draw(EditorUI* editor, Scene* scene, ResourceManager* resources, Project* project);

private:
    // Members (rather than free functions) to benefit from EditorUI's friendship.
    static void refreshAssetList(EditorUI* editor, Project* project);
    static void drawClipViewSection(EditorUI* editor, Animator* animator,
                                    ResourceManager* resources, Project* project);
    static void drawAssetsSection(EditorUI* editor, Animator* animator,
                                  ResourceManager* resources, Project* project);
    static void completePendingAssetLoad(EditorUI* editor, Animator* animator,
                                         ResourceManager* resources);
    static void drawGraphSection(EditorUI* editor, Animator* animator,
                                 ResourceManager* resources);
    static void drawSequenceSection(EditorUI* editor);
};

} // namespace saida
