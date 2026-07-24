#pragma once

#include <memory>
#include <string>

namespace saida {

class Project;
class ResourceManager;
class Scene;
class Animator;

// Owns the model-preview scene and the importer window lifetime. While active,
// previewScene() is the exact scene installed as EditorApp's scene override.
class ModelImporterPanel {
public:
    ~ModelImporterPanel();

    void open(const std::string& path, Project* project,
              ResourceManager* resources);
    void close();
    void draw();

    bool active() const { return active_; }
    Scene* previewScene() const { return previewScene_.get(); }

private:
    Animator* findAnimator() const;
    void drawAnimationControls(Animator* animator);
    void drawExport();

    bool active_ = false;
    std::string modelPath_;
    std::unique_ptr<Scene> previewScene_;
    std::string exportStatus_;
    float playbackSpeed_ = 1.0f;
};

} // namespace saida
