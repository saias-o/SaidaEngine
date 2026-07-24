#pragma once

namespace saida {

class EditorUI;
class Project;
class Scene;

class MenuBarPanel {
public:
    void draw(EditorUI* editor, Project* project, Scene* scene);
};

} // namespace saida
