#pragma once

namespace saida {

class EditorUI;
class Project;
class ResourceManager;

class FileBrowserPanel {
public:
    void draw(EditorUI* editor, Project* project, ResourceManager* resources);
};

} // namespace saida
