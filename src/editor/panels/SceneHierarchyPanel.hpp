#pragma once

namespace saida {

class EditorUI;
class Scene;
class Node;

class SceneHierarchyPanel {
public:
    void draw(EditorUI* editor, Scene* scene);
private:
    void drawSceneTreeNode(EditorUI* editor, Node* node);
};

} // namespace saida
