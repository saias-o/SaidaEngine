#pragma once

#include "editor/BuildController.hpp"
#include "editor/CommandHistory.hpp"
#include "editor/EditorEnums.hpp"
#include "editor/GizmoController.hpp"
#include "editor/panels/ModelImporterPanel.hpp"
#include "editor/ProjectDialogs.hpp"
#include "editor/SceneDocument.hpp"
#include "editor/SettingsWindow.hpp"
#include "editor/ThumbnailCache.hpp"
#include "scene/animation/AnimationSequence.hpp"

#include <any>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace saida {

class Scene;
class Camera;
class Node;
class Project;
class ResourceManager;

class MenuBarPanel;
class SceneHierarchyPanel;
class InspectorPanel;
class FileBrowserPanel;
class ViewportPanel;
class ProfilerPanel;
class AnimationPanel;
class McpBridge;

class EditorApp;

class EditorUI {
    friend class MenuBarPanel;
    friend class SceneHierarchyPanel;
    friend class InspectorPanel;
    friend class FileBrowserPanel;
    friend class ViewportPanel;
    friend class ProfilerPanel;
    friend class AnimationPanel;
    friend class PropertyEditor;
    friend class McpBridge;
    friend class GizmoController;
public:
    EditorUI();
    ~EditorUI();

    void draw(EditorApp* app, Scene* scene, Camera* camera, Project* project, ResourceManager* resources, float dt);

    Node* selectedNode() const { return selectedNode_; }
    Project* ctxProject() const { return ctxProject_; }

    void clearSelection() { selectedNode_ = nullptr; document_.clearSelection(); }

    bool quitRequested() const { return quitRequested_; }

    // Automated Build click (--build flag): same code as the Build dialog's
    // button. `message` receives the output folder or the error.
    bool runAutomatedBuild(Project* project, bool web, const std::string& outputDir,
                           std::string* message);

    bool isViewportHovered(float mx, float my) const {
        return mx >= viewportPos_.x && mx <= viewportPos_.x + viewportSize_.x &&
               my >= viewportPos_.y && my <= viewportPos_.y + viewportSize_.y;
    }
    glm::vec2 viewportPosition() const { return viewportPos_; }
    glm::vec2 viewportSize() const { return viewportSize_; }

    void openModelImporter(const std::string& path, ResourceManager* resources);

    bool isPreviewMode() const { return modelImporter_.active(); }
    Scene* previewScene() const { return modelImporter_.previewScene(); }

    // Play is inspectable but read-only: no editor mutation may touch the live
    // World sub-scene. Every command and inspector edit is gated on this.
    bool canEdit() const;

    void markDirty();

private:
    // Play mode rejects edits so the live scene cannot diverge from the document.
    void execute(std::unique_ptr<Command> command);

    void saveScene(const std::string& path);
    void loadScene(const std::string& path);
    void copySelected();
    void pasteClipboard();
    void duplicateSelected();

    void beginFrameContext(EditorApp* app, Scene* scene, Camera* camera,
                           Project* project, ResourceManager* resources);
    void handleShortcuts(Project* project);
    void drawDockspace();
    void drawPanels(Scene* scene, Camera* camera, Project* project,
                    ResourceManager* resources, float dt);
    void drawModals(Project* project);
    void drawAboutWindow();

    bool showSceneTree_   = true;
    bool showInspector_   = true;
    bool showFileBrowser_ = true;
    bool showViewportOverlay_ = true;
    bool showProfiler_ = false;
    bool showAnimation_ = false;

    // Animation panel — state survives across frames (panels are recreated).
    char animViewName_[128] = "NewView";
    int animViewSourceIndex_ = 0;
    float animViewStart_ = 0.0f;
    float animViewEnd_ = 0.0f;
    bool animViewLoop_ = true;
    float animViewSpeed_ = 1.0f;
    std::string animStatus_;
    std::vector<std::string> animAssetFiles_;  // .srig/.sclip/.sgraph/.sseq (relative paths)
    double animAssetScanTime_ = -1.0;
    uint64_t animAppliedGraphId_ = 0;  // AssetID of the applied .sgraph (0 = none)
    enum class AnimPendingAction {
        None,
        PlayClipView,
        EditClipView,
        ApplyGraph,
        InspectRig
    };
    AnimPendingAction animPendingAction_ = AnimPendingAction::None;
    uint64_t animPendingAssetId_ = 0;
    std::string animPendingAssetPath_;

    // Sequence in preview: the player survives across frames, the bound Animator
    // carries the track node as long as playback is active.
    std::unique_ptr<SequencePlayer> animSequencePlayer_;
    std::string animSequencePath_;

    Node* selectedNode_ = nullptr;

    bool quitRequested_ = false;
    bool dockLayoutBuilt_ = false;

    std::string currentBrowsePath_;
    float fileBrowserZoom_ = 1.0f;

    // Cache listings to avoid disk access every frame.
    struct FileListing {
        std::string path;
        std::string query;
        double time = -1.0;
        std::vector<std::string> dirs;
        std::vector<std::string> files;
    };
    FileListing fileListing_;

    bool showAboutWindow_       = false;
    void rebuildSceneHierarchy(Scene* scene);

    std::string resolveScenePath(Project* project) const;

    BuildController buildController_;
    ModelImporterPanel modelImporter_;
    ProjectDialogs projectDialogs_;
    SettingsWindow settings_;

    // Deferral avoids iterator invalidation during UI traversal.
    Node* nodeToDelete_ = nullptr;
    Node* nodeToCreateChildUnder_ = nullptr;
    Node* nodeToCreateParentFor_ = nullptr;
    CreateNodeType createType_ = CreateNodeType::None;
    CreateNodeType createParentType_ = CreateNodeType::None;
    std::string draggedScenePath_;

    Node* nodeToReparent_ = nullptr;
    Node* newParent_ = nullptr;
    size_t newChildIndex_ = static_cast<size_t>(-1);

    Node* nodeToRename_ = nullptr;
    char nodeRenameBuf_[128] = "";

    std::string fileToRename_;
    char fileRenameBuf_[256] = "";

    char sceneTreeSearchBuf_[128] = "";
    char fileBrowserSearchBuf_[128] = "";

    // Active tool mode is shared editor state: set by the viewport toolbar and
    // by keyboard, consumed by the gizmo. The manipulation/geometry state lives
    // in GizmoController.
    GizmoMode gizmoMode_ = GizmoMode::Translate;
    GizmoController gizmo_;

    glm::vec2 viewportPos_{0.0f, 0.0f};
    glm::vec2 viewportSize_{0.0f, 0.0f};

    SceneDocument document_;
    CommandHistory history_;

    // A single ImGui widget is active, so one transactional edit slot suffices.
    unsigned int propEditId_ = 0;
    std::any propEditOld_;

    // A project change invalidates history and clipboard node references.
    uint64_t lastProjectVersion_ = 0;
    EditorApp* app_ = nullptr;
    Scene* ctxScene_ = nullptr;
    Camera* ctxCamera_ = nullptr;
    ResourceManager* ctxResources_ = nullptr;
    Project* ctxProject_ = nullptr;
    ThumbnailCache thumbnails_;
    ThumbnailCache brandingImages_;

#ifdef SAIDA_ENABLE_MCP
    std::unique_ptr<McpBridge> mcp_;
#endif
};

} // namespace saida
