#pragma once

namespace saida {

class EditorUI;
class Camera;

class ViewportPanel {
public:
    void draw(EditorUI* editor, Camera* camera, float dt);
};

} // namespace saida
