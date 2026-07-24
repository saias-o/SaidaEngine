#pragma once

// Manipulation gizmo for the selected node (translate/rotate/scale) plus the
// collider wireframe overlay. Owns the drag transaction and the per-frame
// screen-space geometry cache; reads/writes the shared editor state
// (selection, viewport rect, tool mode, command stack) through EditorUI.
//
// Invariant: grabbedAxis_ != None  ⟺  a drag is in progress, and the dragStart*
// snapshot holds the node transform captured at grab time — the baseline the
// released drag turns into a single undoable TransformCommand.

#include "editor/EditorEnums.hpp"  // GizmoMode, GizmoAxis

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

struct ImDrawList;

namespace saida {

class EditorUI;
class Camera;
class Scene;
class CollisionShapeNode;

class GizmoController {
public:
    // Transform gizmo for the active selection. No-op in play mode or with no
    // selection. Also performs click-to-select raycasting on empty clicks.
    void draw(EditorUI& editor, Camera* camera, Scene* scene);

    // Green wireframe of every CollisionShape (gated on Project::showColliders).
    void drawColliders(EditorUI& editor, Camera* camera, Scene* scene);

private:
    bool buildScreenGeometry(EditorUI& editor, Camera& camera,
                             const glm::mat4& viewProjection);
    void updateDragTransaction(EditorUI& editor, Scene& scene,
                               const glm::vec3& rayOrigin,
                               const glm::vec3& rayDirection,
                               const glm::vec2& mousePosition,
                               int hoveredAxis, bool mouseDown,
                               bool mouseClicked);
    void updateHover(EditorUI& editor, const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                     const glm::vec2& mousePos, int& outHoveredAxis);
    void handleDrag(EditorUI& editor, const glm::vec3& rayOrigin,
                    const glm::vec3& rayDir, const glm::vec2& mousePos);
    void performRaycastSelection(EditorUI& editor, Scene* scene,
                                 const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                                 const glm::vec2& mousePos);
    void renderRotationRings(EditorUI& editor, ImDrawList* drawList, Camera* camera,
                             const glm::mat4& viewProj, int hoveredAxis);
    void renderTranslateScale(EditorUI& editor, ImDrawList* drawList, int hoveredAxis);
    void drawColliderShape(CollisionShapeNode& shape,
                           const glm::mat4& viewProjection,
                           const glm::vec2& viewportPosition,
                           const glm::vec2& viewportSize,
                           ImDrawList* drawList);

    // Drag transaction state (see invariant above).
    GizmoAxis grabbedAxis_ = GizmoAxis::None;
    glm::vec3 dragStartNodePos_{0.0f};
    glm::vec3 dragStartNodeRotEuler_{0.0f};
    glm::quat dragStartNodeRotQuat_{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 dragStartNodeScale_{1.0f};
    glm::vec2 dragStartMousePos_{0.0f};

    // Rotation drag (screen-space tangential): angle accumulated since grab, the
    // previous mouse position for the per-frame delta, and the screen->axis sign
    // fixed at grab time from the ring's facing toward the camera. Screen-space
    // tangential motion stays responsive even when the ring is edge-on, where a
    // ray/plane hit test degenerates.
    float rotationAccumAngle_{0.0f};
    glm::vec2 rotationLastMouse_{0.0f};
    float rotationScreenSign_{1.0f};
    // Normalized world rotation axis, frozen at grab. Reusing the live
    // gizmoLocalAxes_ (rebuilt from the rotation we just wrote) feeds a
    // non-unit axis back into angleAxis and compounds into NaN.
    glm::vec3 rotationAxis_{0.0f, 1.0f, 0.0f};

    // Translate/scale drag basis, frozen at grab time so the mapping stays linear
    // and reversible. Recomputing it from the moving gizmo each frame feeds the
    // object's motion back into its own sensitivity and runs away. Translate uses
    // the axis parameter of the point on the axis closest to the mouse ray;
    // scale uses a fixed screen-space direction and world-units-per-pixel scale.
    float dragStartAxisParam_{0.0f};
    glm::vec2 dragStartAxisDir2D_{0.0f};
    float dragStartWorldPerPixel_{0.0f};

    // Per-frame screen-space geometry cache (rebuilt at the top of draw()).
    glm::vec3 gizmoNodePos_{0.0f};
    float gizmoWorldLength_{0.0f};
    glm::vec2 gizmoCenter2D_{0.0f};
    glm::vec2 gizmoEnds2D_[3];
    glm::vec3 gizmoLocalAxes_[3];
    bool gizmoAxisValid_[3]{false, false, false};

    // Screen position of the last pick, so repeated clicks at the same spot cycle
    // through the overlapping candidates under the ray (Unity-style drill-down).
    glm::vec2 lastPickScreenPos_{-1.0e6f, -1.0e6f};
};

} // namespace saida
