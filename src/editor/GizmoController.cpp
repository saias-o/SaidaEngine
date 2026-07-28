#include "editor/GizmoController.hpp"

#include "editor/EditorUI.hpp"
#include "editor/EditorApp.hpp"
#include "editor/Command.hpp"
#include "core/Camera.hpp"
#include "scene/Node.hpp"
#include "scene/Scene.hpp"
#include "project/Project.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "physics/CollisionObjectNode.hpp"

#include "imgui.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace saida {

namespace {

bool intersectRayPlane(const glm::vec3& rayOrigin, const glm::vec3& rayDir, const glm::vec3& planeOrigin, const glm::vec3& planeNormal, float& t) {
    float denom = glm::dot(planeNormal, rayDir);
    if (std::abs(denom) > 1e-6f) {
        glm::vec3 p0l0 = planeOrigin - rayOrigin;
        t = glm::dot(p0l0, planeNormal) / denom;
        return (t >= 0.0f);
    }
    return false;
}

// Signed distance, along a world axis from axisPoint, of the point on that axis
// line closest to the mouse ray. Used to translate so the grabbed axis point
// tracks the cursor exactly. Returns a non-finite value when the axis is nearly
// parallel to the ray (the drag is then ambiguous and should be skipped).
float closestAxisParam(const glm::vec3& axisPoint, const glm::vec3& axisDir,
                       const glm::vec3& rayOrigin, const glm::vec3& rayDir) {
    float b = glm::dot(axisDir, rayDir);
    float denom = 1.0f - b * b;
    if (std::abs(denom) < 1e-5f) return std::numeric_limits<float>::quiet_NaN();
    glm::vec3 w0 = axisPoint - rayOrigin;
    float d = glm::dot(axisDir, w0);
    float e = glm::dot(rayDir, w0);
    return (b * e - d) / denom;
}

// Helper function to calculate distance from point to segment in 2D
float distanceToSegment(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b) {
    glm::vec2 ab = b - a;
    float l2 = ab.x * ab.x + ab.y * ab.y;
    if (l2 == 0.0f) return glm::length(p - a);
    float t = ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / l2;
    t = glm::clamp(t, 0.0f, 1.0f);
    glm::vec2 projection = a + t * ab;
    return glm::length(p - projection);
}

namespace GizmoConfig {
    constexpr float RotationSensitivity = 50.0f;
    constexpr float RingThicknessRatio = 0.08f;
    constexpr int RingSegments = 64;
    constexpr float SelectionThresholdTranslate = 10.0f;
    constexpr float SelectionThresholdHead = 18.0f;
    constexpr float GizmoScreenScale = 0.15f;
    constexpr float MinWorldLength = 0.01f;

    constexpr ImU32 ColorX = IM_COL32(239, 68, 68, 255);
    constexpr ImU32 ColorY = IM_COL32(16, 185, 129, 255);
    constexpr ImU32 ColorZ = IM_COL32(59, 130, 246, 255);

    constexpr ImU32 HoverColorX = IM_COL32(252, 165, 165, 255);
    constexpr ImU32 HoverColorY = IM_COL32(110, 231, 183, 255);
    constexpr ImU32 HoverColorZ = IM_COL32(147, 197, 253, 255);

    constexpr float LineThicknessDefault = 2.5f;
    constexpr float LineThicknessHover = 4.0f;
    constexpr float RingThicknessDefault = 3.0f;
    constexpr float RingThicknessHover = 5.0f;
}

constexpr float kPi = 3.14159265358979f;

// Project a world-space point to viewport screen coordinates; false if behind.
bool projectPoint(const glm::mat4& viewProj, const glm::vec2& vpPos, const glm::vec2& vpSize,
                  const glm::vec3& world, ImVec2& out) {
    glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
    if (clip.w <= 1e-4f) return false;
    glm::vec3 ndc = glm::vec3(clip) / clip.w;
    out = ImVec2(vpPos.x + (ndc.x + 1.0f) * 0.5f * vpSize.x,
                 vpPos.y + (ndc.y + 1.0f) * 0.5f * vpSize.y);
    return true;
}

bool colliderBodyTransform(CollisionShapeNode& shape, Node*& bodyNode,
                           glm::mat4& transform) {
    for (Node* parent = shape.parent(); parent; parent = parent->parent()) {
        if (parent->asCollisionObject()) {
            bodyNode = parent;
            break;
        }
    }
    if (!bodyNode) return false;

    glm::mat4 world = bodyNode->worldTransform();
    glm::vec3 c0(world[0]), c1(world[1]), c2(world[2]);
    glm::vec3 scale(glm::length(c0), glm::length(c1), glm::length(c2));
    if (scale.x < 1e-6f) scale.x = 1.0f;
    if (scale.y < 1e-6f) scale.y = 1.0f;
    if (scale.z < 1e-6f) scale.z = 1.0f;
    glm::quat rotation = glm::normalize(glm::quat_cast(
        glm::mat3(c0 / scale.x, c1 / scale.y, c2 / scale.z)));
    transform = glm::translate(glm::mat4(1.0f), glm::vec3(world[3])) *
                glm::mat4_cast(rotation);
    return true;
}

} // namespace

void GizmoController::draw(EditorUI& editor, Camera* camera, Scene* scene) {
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_T)) editor.gizmoMode_ = GizmoMode::Translate;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) editor.gizmoMode_ = GizmoMode::Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_S)) editor.gizmoMode_ = GizmoMode::Scale;
    }

    if ((editor.app_ && editor.app_->isPlayMode()) || !camera || !scene) {
        grabbedAxis_ = GizmoAxis::None;
        return;
    }

    const ImVec2 imMousePos = ImGui::GetMousePos();
    const glm::vec2 mousePos(imMousePos.x, imMousePos.y);
    glm::mat4 viewProj = camera->projection() * camera->view();
    glm::mat4 invVP = glm::inverse(viewProj);
    float ndcX = ((mousePos.x - editor.viewportPos_.x) / editor.viewportSize_.x) * 2.0f - 1.0f;
    float ndcY = ((mousePos.y - editor.viewportPos_.y) / editor.viewportSize_.y) * 2.0f - 1.0f;
    glm::vec4 nearW = invVP * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    glm::vec4 farW = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    glm::vec3 rayOrigin = glm::vec3(nearW) / nearW.w;
    glm::vec3 rayDir = glm::normalize((glm::vec3(farW) / farW.w) - rayOrigin);

    // Picking must work even with nothing selected yet: without a selection the
    // gizmo has no geometry to build, draw, or drag, but a left-click in the
    // viewport should still pick the object under the cursor. The rest of the
    // gizmo flow below requires a selected node.
    if (!editor.selectedNode_) {
        grabbedAxis_ = GizmoAxis::None;
        if (!ImGui::GetIO().WantCaptureMouse &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            performRaycastSelection(editor, scene, rayOrigin, rayDir, mousePos);
        }
        return;
    }

    if (!buildScreenGeometry(editor, *camera, viewProj)) return;
    int hoveredAxis = -1;
    updateHover(editor, rayOrigin, rayDir, mousePos, hoveredAxis);
    updateDragTransaction(
        editor, *scene, rayOrigin, rayDir, mousePos, hoveredAxis,
        ImGui::IsMouseDown(ImGuiMouseButton_Left),
        ImGui::IsMouseClicked(ImGuiMouseButton_Left));

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    if (editor.gizmoMode_ == GizmoMode::Rotate)
        renderRotationRings(editor, drawList, camera, viewProj, hoveredAxis);
    else
        renderTranslateScale(editor, drawList, hoveredAxis);

    drawList->AddCircleFilled(ImVec2(gizmoCenter2D_.x, gizmoCenter2D_.y), 5.0f, ImColor(255, 255, 255, 220));
    drawList->AddCircle(ImVec2(gizmoCenter2D_.x, gizmoCenter2D_.y), 5.0f, ImColor(0, 0, 0, 255), 0, 1.0f);
}

bool GizmoController::buildScreenGeometry(EditorUI& editor, Camera& camera,
                                          const glm::mat4& viewProj) {
    gizmoNodePos_ = editor.selectedNode_->transform().position;
    glm::quat nodeRot = editor.selectedNode_->transform().rotation;

    gizmoLocalAxes_[0] = (editor.gizmoMode_ == GizmoMode::Rotate) ? nodeRot * glm::vec3(1,0,0) : glm::vec3(1,0,0);
    gizmoLocalAxes_[1] = (editor.gizmoMode_ == GizmoMode::Rotate) ? nodeRot * glm::vec3(0,1,0) : glm::vec3(0,1,0);
    gizmoLocalAxes_[2] = (editor.gizmoMode_ == GizmoMode::Rotate) ? nodeRot * glm::vec3(0,0,1) : glm::vec3(0,0,1);

    glm::vec4 clipCenter = viewProj * glm::vec4(gizmoNodePos_, 1.0f);
    if (clipCenter.w <= 0.0f) return false;

    glm::vec3 ndcCenter = glm::vec3(clipCenter) / clipCenter.w;
    gizmoCenter2D_ = glm::vec2(editor.viewportPos_.x + (ndcCenter.x + 1.0f) * 0.5f * editor.viewportSize_.x, editor.viewportPos_.y + (ndcCenter.y + 1.0f) * 0.5f * editor.viewportSize_.y);

    gizmoWorldLength_ = std::max(
        GizmoConfig::MinWorldLength,
        glm::length(camera.position - gizmoNodePos_) *
            GizmoConfig::GizmoScreenScale);

    for (int i = 0; i < 3; ++i) {
        glm::vec3 axisEnd3D = gizmoNodePos_ + gizmoLocalAxes_[i] * gizmoWorldLength_;
        glm::vec4 clipEnd = viewProj * glm::vec4(axisEnd3D, 1.0f);
        if (clipEnd.w > 0.0f) {
            glm::vec3 ndcEnd = glm::vec3(clipEnd) / clipEnd.w;
            gizmoEnds2D_[i] = glm::vec2(editor.viewportPos_.x + (ndcEnd.x + 1.0f) * 0.5f * editor.viewportSize_.x, editor.viewportPos_.y + (ndcEnd.y + 1.0f) * 0.5f * editor.viewportSize_.y);
            gizmoAxisValid_[i] = true;
        } else {
            gizmoAxisValid_[i] = false;
        }
    }
    return true;
}

void GizmoController::updateDragTransaction(
    EditorUI& editor, Scene& scene, const glm::vec3& rayOrigin,
    const glm::vec3& rayDir, const glm::vec2& mousePos, int hoveredAxis,
    bool isMouseDown, bool isMouseClicked) {
    if (isMouseClicked && hoveredAxis != -1 &&
        grabbedAxis_ == GizmoAxis::None) {
        grabbedAxis_ = static_cast<GizmoAxis>(hoveredAxis);
        dragStartNodePos_ = gizmoNodePos_;
        dragStartNodeRotEuler_ = glm::degrees(glm::eulerAngles(editor.selectedNode_->transform().rotation));
        glm::quat startRot = editor.selectedNode_->transform().rotation;
        // Recover a previously corrupted (NaN / zero-length) rotation instead of
        // propagating it: a grab resets such a node back to identity.
        if (!std::isfinite(startRot.x) || !std::isfinite(startRot.y) ||
            !std::isfinite(startRot.z) || !std::isfinite(startRot.w) ||
            glm::length(startRot) < 1e-6f) {
            startRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            editor.selectedNode_->transform().rotation = startRot;
        }
        dragStartNodeRotQuat_ = glm::normalize(startRot);
        dragStartNodeScale_ = editor.selectedNode_->transform().scale;
        dragStartMousePos_ = mousePos;

        if (editor.gizmoMode_ == GizmoMode::Rotate) {
            rotationAccumAngle_ = 0.0f;
            rotationLastMouse_ = mousePos;
            glm::vec3 axis = gizmoLocalAxes_[hoveredAxis];
            float axisLen = glm::length(axis);
            rotationAxis_ = (axisLen > 1e-6f) ? axis / axisLen : glm::vec3(0.0f, 1.0f, 0.0f);
            // Map a screen-space CCW drag to a rotation the user sees in the same
            // sense: invert when the ring's axis points toward the camera.
            // -rayDir points from the cursor back toward the camera.
            float facing = glm::dot(rotationAxis_, -rayDir);
            rotationScreenSign_ = (facing >= 0.0f) ? -1.0f : 1.0f;
        } else {
            // Freeze the translate/scale drag basis at grab (see header note).
            glm::vec3 axisWorld = gizmoLocalAxes_[hoveredAxis];
            dragStartAxisParam_ = closestAxisParam(dragStartNodePos_, axisWorld, rayOrigin, rayDir);
            glm::vec2 dir2D = gizmoEnds2D_[hoveredAxis] - gizmoCenter2D_;
            float len2D = glm::length(dir2D);
            dragStartAxisDir2D_ = (len2D > 1.0f) ? dir2D / len2D : glm::vec2(1.0f, 0.0f);
            dragStartWorldPerPixel_ = (len2D > 1.0f) ? gizmoWorldLength_ / len2D : 0.0f;
        }
    }

    if (grabbedAxis_ != GizmoAxis::None && isMouseDown) {
        handleDrag(editor, rayOrigin, rayDir, mousePos);
    } else if (!isMouseDown) {
        // Drag released: record the net move (start → final) as one undoable,
        // dirty-marking command instead of leaving a silent direct mutation.
        if (grabbedAxis_ != GizmoAxis::None && editor.selectedNode_) {
            Transform oldT;
            oldT.position = dragStartNodePos_;
            oldT.rotation = dragStartNodeRotQuat_;
            oldT.scale    = dragStartNodeScale_;
            const Transform& newT = editor.selectedNode_->transform();
            bool changed = glm::distance(oldT.position, newT.position) > 1e-6f
                        || glm::distance(oldT.scale, newT.scale) > 1e-6f
                        || std::abs(glm::dot(oldT.rotation, newT.rotation)) < 0.999999f;
            if (changed)
                editor.execute(std::make_unique<TransformCommand>(editor.selectedNode_->id(), oldT, newT));
        }
        grabbedAxis_ = GizmoAxis::None;
    }

    if (!ImGui::GetIO().WantCaptureMouse && isMouseClicked &&
        hoveredAxis == -1 && grabbedAxis_ == GizmoAxis::None) {
        performRaycastSelection(editor, &scene, rayOrigin, rayDir, mousePos);
    }
}

void GizmoController::updateHover(EditorUI& editor, const glm::vec3& rayOrigin, const glm::vec3& rayDir, const glm::vec2& mousePos, int& outHoveredAxis) {
    if (ImGui::GetIO().WantCaptureMouse && grabbedAxis_ == GizmoAxis::None) return;

    if (editor.gizmoMode_ == GizmoMode::Rotate) {
        float closestDistToRing = 9999.0f;
        for (int i = 0; i < 3; ++i) {
            float t;
            if (intersectRayPlane(rayOrigin, rayDir, gizmoNodePos_, gizmoLocalAxes_[i], t)) {
                glm::vec3 hitPos = rayOrigin + rayDir * t;
                float distFromCenter = glm::length(hitPos - gizmoNodePos_);
                if (std::abs(distFromCenter - gizmoWorldLength_) < gizmoWorldLength_ * GizmoConfig::RingThicknessRatio) {
                    if (t < closestDistToRing) {
                        closestDistToRing = t;
                        outHoveredAxis = i;
                    }
                }
            }
        }
    } else {
        float closestDist = 9999.0f;
        for (int i = 0; i < 3; ++i) {
            if (!gizmoAxisValid_[i]) continue;
            float dSeg = distanceToSegment(mousePos, gizmoCenter2D_, gizmoEnds2D_[i]);
            float dHead = glm::length(mousePos - gizmoEnds2D_[i]);
            if (dSeg < GizmoConfig::SelectionThresholdTranslate || dHead < GizmoConfig::SelectionThresholdHead) {
                if (dSeg < closestDist) {
                    closestDist = dSeg;
                    outHoveredAxis = i;
                }
            }
        }
    }
}

void GizmoController::handleDrag(EditorUI& editor, const glm::vec3& rayOrigin,
                                 const glm::vec3& rayDir, const glm::vec2& mousePos) {
    int axis = static_cast<int>(grabbedAxis_);
    const glm::vec3 axisWorld = gizmoLocalAxes_[axis];

    if (editor.gizmoMode_ == GizmoMode::Rotate) {
        // Accumulate the signed screen-space angle swept around the gizmo centre
        // and apply it around the axis frozen (and normalized) at grab. This stays
        // live at any viewing angle — including an edge-on ring, where a ray/plane
        // hit test degenerates — and the fixed unit axis plus a normalized result
        // keep the quaternion stable instead of compounding into NaN.
        const glm::vec2 vPrev = rotationLastMouse_ - gizmoCenter2D_;
        const glm::vec2 vCurr = mousePos - gizmoCenter2D_;
        if (glm::length(vPrev) > 2.0f && glm::length(vCurr) > 2.0f) {
            float cross = vPrev.x * vCurr.y - vPrev.y * vCurr.x;
            float dot = vPrev.x * vCurr.x + vPrev.y * vCurr.y;
            rotationAccumAngle_ += std::atan2(cross, dot) * rotationScreenSign_;
            editor.selectedNode_->transform().rotation = glm::normalize(
                glm::angleAxis(rotationAccumAngle_, rotationAxis_) * dragStartNodeRotQuat_);
        }
        rotationLastMouse_ = mousePos;
    } else if (editor.gizmoMode_ == GizmoMode::Translate) {
        // Move the object so the grabbed axis point stays under the cursor: the
        // delta is the change in the axis parameter of the point on the axis
        // closest to the mouse ray, relative to grab. The basis is fixed at grab,
        // so the mapping is linear and dragging back returns exactly to start.
        float param = closestAxisParam(dragStartNodePos_, axisWorld, rayOrigin, rayDir);
        if (std::isfinite(param)) {
            editor.selectedNode_->transform().position =
                dragStartNodePos_ + axisWorld * (param - dragStartAxisParam_);
        }
    } else if (editor.gizmoMode_ == GizmoMode::Scale) {
        // Screen-space drag along the axis using the direction and world-per-pixel
        // scale frozen at grab (constant sensitivity, reversible).
        float screenProj = glm::dot(mousePos - dragStartMousePos_, dragStartAxisDir2D_);
        float scaleDelta = screenProj * dragStartWorldPerPixel_ * 0.5f;
        glm::vec3 newScale = dragStartNodeScale_;
        if (axis == 0) newScale.x += scaleDelta;
        else if (axis == 1) newScale.y += scaleDelta;
        else if (axis == 2) newScale.z += scaleDelta;
        editor.selectedNode_->transform().scale = newScale;
    }
}

void GizmoController::performRaycastSelection(EditorUI& editor, Scene* scene, const glm::vec3& rayOrigin, const glm::vec3& rayDir, const glm::vec2& mousePos) {
    // Collect every node the ray passes through, nearest first, so repeated
    // clicks at the same spot can walk through overlapping candidates.
    struct Hit { float t; Node* node; };
    std::vector<Hit> hits;
    scene->traverse([&](Node& n, const glm::mat4& worldMatrix) {
        if (!n.parent()) return;
        glm::vec3 objWorldPos = glm::vec3(worldMatrix[3]);
        float sx = glm::length(glm::vec3(worldMatrix[0]));
        float sy = glm::length(glm::vec3(worldMatrix[1]));
        float sz = glm::length(glm::vec3(worldMatrix[2]));
        float maxScale = glm::max(sx, glm::max(sy, sz));

        float radius = 0.5f * maxScale;
        if (n.mesh()) radius = 1.0f * maxScale;
        else if (n.asLightConst()) radius = 0.6f * maxScale;

        glm::vec3 oc = rayOrigin - objWorldPos;
        float b = glm::dot(rayDir, oc);
        float c = glm::dot(oc, oc) - radius * radius;
        float discriminant = b * b - c;

        if (discriminant >= 0.0f) {
            float t = -b - glm::sqrt(discriminant);
            if (t > 0.0f) hits.push_back({t, &n});
        }
    });

    if (hits.empty()) {
        editor.selectedNode_ = nullptr;
        lastPickScreenPos_ = mousePos;
        return;
    }

    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.t < b.t; });

    // Same spot as the previous click and the current selection is one of the
    // candidates → advance to the next one behind it (wrapping). Otherwise start
    // from the nearest. This is Unity's "click again to select the object/child
    // underneath" behaviour.
    Node* pick = hits.front().node;
    if (glm::length(mousePos - lastPickScreenPos_) < 4.0f && editor.selectedNode_) {
        for (size_t i = 0; i < hits.size(); ++i) {
            if (hits[i].node == editor.selectedNode_) {
                pick = hits[(i + 1) % hits.size()].node;
                break;
            }
        }
    }
    editor.selectedNode_ = pick;
    lastPickScreenPos_ = mousePos;
}

void GizmoController::renderRotationRings(EditorUI& editor, ImDrawList* drawList, Camera* camera, const glm::mat4& viewProj, int hoveredAxis) {
    ImU32 colors[3] = { GizmoConfig::ColorX, GizmoConfig::ColorY, GizmoConfig::ColorZ };
    ImU32 hoverColors[3] = { GizmoConfig::HoverColorX, GizmoConfig::HoverColorY, GizmoConfig::HoverColorZ };

    const glm::vec2 vpPos = editor.viewportPos_;
    const glm::vec2 vpSize = editor.viewportSize_;

    // Outer camera-facing "trackball" circle (Unity/Godot look). Its screen
    // radius is the projected world radius of the rings, nudged out slightly.
    ImVec2 centerScreen, edgeScreen;
    if (projectPoint(viewProj, vpPos, vpSize, gizmoNodePos_, centerScreen) &&
        projectPoint(viewProj, vpPos, vpSize,
                     gizmoNodePos_ + camera->right() * gizmoWorldLength_, edgeScreen)) {
        float screenRadius = glm::length(glm::vec2(edgeScreen.x - centerScreen.x,
                                                   edgeScreen.y - centerScreen.y));
        drawList->AddCircle(centerScreen, screenRadius * 1.18f,
                            IM_COL32(210, 210, 210, 90), GizmoConfig::RingSegments, 1.5f);
    }

    // Rings follow the node's live orientation (gizmoLocalAxes_ is rebuilt from
    // the current rotation each frame), so the gizmo visibly turns while
    // dragging instead of freezing at the grab pose.
    for (int i = 0; i < 3; ++i) {
        bool isActive = (hoveredAxis == i || static_cast<int>(grabbedAxis_) == i);
        ImU32 col = isActive ? hoverColors[i] : colors[i];
        ImVec4 colV = ImGui::ColorConvertU32ToFloat4(col);
        ImU32 backCol = ImGui::ColorConvertFloat4ToU32(ImVec4(colV.x, colV.y, colV.z, colV.w * 0.18f));
        float frontThickness = isActive ? GizmoConfig::RingThicknessHover
                                        : GizmoConfig::RingThicknessDefault;

        glm::vec3 normal = gizmoLocalAxes_[i];
        glm::vec3 u = glm::normalize(glm::cross(normal, std::abs(normal.x) > 0.9f ? glm::vec3(0,1,0) : glm::vec3(1,0,0)));
        glm::vec3 v = glm::cross(normal, u);

        // Walk the ring segment by segment so a front arc is never joined to a
        // far arc by a stray chord across the circle. Front half (facing the
        // camera) draws bold; the back half stays faint for depth reading.
        ImVec2 prev{};
        bool prevOk = false;
        bool prevFront = false;
        for (int s = 0; s <= GizmoConfig::RingSegments; ++s) {
            float theta = (static_cast<float>(s) / GizmoConfig::RingSegments) * glm::two_pi<float>();
            glm::vec3 pos3D = gizmoNodePos_ + (u * std::cos(theta) + v * std::sin(theta)) * gizmoWorldLength_;
            ImVec2 p2d;
            bool ok = projectPoint(viewProj, vpPos, vpSize, pos3D, p2d);
            bool front = glm::dot(glm::normalize(pos3D - camera->position),
                                  glm::normalize(pos3D - gizmoNodePos_)) <= 0.0f;
            if (ok && prevOk) {
                bool solid = front && prevFront;
                drawList->AddLine(prev, p2d, solid ? col : backCol,
                                  solid ? frontThickness : 1.25f);
            }
            prev = p2d;
            prevOk = ok;
            prevFront = front;
        }
    }
}

void GizmoController::renderTranslateScale(EditorUI& editor, ImDrawList* drawList, int hoveredAxis) {
    ImU32 colors[3] = { GizmoConfig::ColorX, GizmoConfig::ColorY, GizmoConfig::ColorZ };
    ImU32 hoverColors[3] = { GizmoConfig::HoverColorX, GizmoConfig::HoverColorY, GizmoConfig::HoverColorZ };

    for (int i = 0; i < 3; ++i) {
        if (!gizmoAxisValid_[i]) continue;

        bool isHovered = (hoveredAxis == i || static_cast<int>(grabbedAxis_) == i);
        ImU32 col = isHovered ? hoverColors[i] : colors[i];
        float thickness = isHovered ? GizmoConfig::LineThicknessHover : GizmoConfig::LineThicknessDefault;

        drawList->AddLine(ImVec2(gizmoCenter2D_.x, gizmoCenter2D_.y), ImVec2(gizmoEnds2D_[i].x, gizmoEnds2D_[i].y), col, thickness);

        if (editor.gizmoMode_ == GizmoMode::Translate) {
            glm::vec2 dir = gizmoEnds2D_[i] - gizmoCenter2D_;
            float dLen = glm::length(dir);
            if (dLen > 1.0f) {
                dir = dir / dLen;
                glm::vec2 perp = glm::vec2(-dir.y, dir.x);
                float headLen = isHovered ? 14.0f : 11.0f;
                float headWidth = isHovered ? 7.0f : 5.5f;
                glm::vec2 p0 = gizmoEnds2D_[i] + dir * headLen;
                glm::vec2 p1 = gizmoEnds2D_[i] - dir * headLen + perp * headWidth;
                glm::vec2 p2 = gizmoEnds2D_[i] - dir * headLen - perp * headWidth;
                drawList->AddTriangleFilled(ImVec2(p0.x, p0.y), ImVec2(p1.x, p1.y), ImVec2(p2.x, p2.y), col);
            }
        } else if (editor.gizmoMode_ == GizmoMode::Scale) {
            float halfSize = isHovered ? 6.0f : 4.5f;
            drawList->AddRectFilled(
                ImVec2(gizmoEnds2D_[i].x - halfSize, gizmoEnds2D_[i].y - halfSize),
                ImVec2(gizmoEnds2D_[i].x + halfSize, gizmoEnds2D_[i].y + halfSize), col
            );
        }
    }
}

void GizmoController::drawColliders(EditorUI& editor, Camera* camera, Scene* scene) {
    if ((editor.app_ && editor.app_->isPlayMode()) || !camera || !scene) return;
    if (editor.ctxProject_ && !editor.ctxProject_->showColliders()) return;

    // The 3D viewport rect is the dock's central node (viewportPos_/
    // viewportSize_), not the whole window — same mapping as the selection
    // gizmo, otherwise colliders drift as soon as panels get docked.
    glm::vec2 vpPos = editor.viewportPos_;
    glm::vec2 vpSize = editor.viewportSize_;
    if (vpSize.x < 1.0f || vpSize.y < 1.0f) return;
    glm::mat4 viewProj = camera->projection() * camera->view();
    scene->traverse([&](Node& n, const glm::mat4&) {
        auto* shape = dynamic_cast<CollisionShapeNode*>(&n);
        if (shape)
            drawColliderShape(*shape, viewProj, vpPos, vpSize,
                              ImGui::GetBackgroundDrawList());
    });
}

void GizmoController::drawColliderShape(
    CollisionShapeNode& shape, const glm::mat4& viewProj,
    const glm::vec2& vpPos, const glm::vec2& vpSize, ImDrawList* drawList) {
    Node* bodyNode = nullptr;
    glm::mat4 bodyTransform;
    if (!colliderBodyTransform(shape, bodyNode, bodyTransform)) return;

    const ImU32 color = IM_COL32(96, 224, 140, 200);
    auto line3D = [&](const glm::vec3& a, const glm::vec3& b) {
        ImVec2 screenA, screenB;
        if (projectPoint(viewProj, vpPos, vpSize, a, screenA) &&
            projectPoint(viewProj, vpPos, vpSize, b, screenB))
            drawList->AddLine(screenA, screenB, color, 1.5f);
    };

    CollisionShapeViz viz =
        shape.resolveViz(glm::inverse(bodyTransform), *bodyNode);
    glm::mat4 primitiveTransform =
        bodyTransform * glm::translate(glm::mat4(1.0f), viz.offset);
    auto transformPoint = [&](const glm::vec3& point) {
        return glm::vec3(primitiveTransform * glm::vec4(point, 1.0f));
    };
    auto arc = [&](const glm::vec3& center, const glm::vec3& u,
                   const glm::vec3& v, float radius, float start, float end,
                   int segments) {
        glm::vec3 previous = transformPoint(
            center + (u * std::cos(start) + v * std::sin(start)) * radius);
        for (int i = 1; i <= segments; ++i) {
            float angle =
                start + (end - start) * (static_cast<float>(i) / segments);
            glm::vec3 current = transformPoint(
                center + (u * std::cos(angle) + v * std::sin(angle)) * radius);
            line3D(previous, current);
            previous = current;
        }
    };

    if (viz.type == CollisionShapeType::Sphere) {
        glm::vec3 X(1, 0, 0), Y(0, 1, 0), Z(0, 0, 1), O(0);
        arc(O, X, Y, viz.radius, 0, 2 * kPi, 28);
        arc(O, X, Z, viz.radius, 0, 2 * kPi, 28);
        arc(O, Y, Z, viz.radius, 0, 2 * kPi, 28);
    } else if (viz.type == CollisionShapeType::Capsule) {
        glm::vec3 axes[3] = {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                             glm::vec3(0, 0, 1)};
        glm::vec3 ax = axes[viz.axis % 3];
        glm::vec3 p1 = axes[(viz.axis + 1) % 3];
        glm::vec3 p2 = axes[(viz.axis + 2) % 3];
        float r = viz.radius;
        float hc = std::max(0.0f, viz.height * 0.5f - r);
        glm::vec3 top = ax * hc, bot = -ax * hc;
        arc(top, p1, p2, r, 0, 2 * kPi, 24);
        arc(bot, p1, p2, r, 0, 2 * kPi, 24);
        for (glm::vec3 direction : {p1, -p1, p2, -p2})
            line3D(transformPoint(top + direction * r),
                   transformPoint(bot + direction * r));
        arc(top, p1, ax, r, 0, kPi, 12);
        arc(top, p2, ax, r, 0, kPi, 12);
        arc(bot, p1, -ax, r, 0, kPi, 12);
        arc(bot, p2, -ax, r, 0, kPi, 12);
    } else {
        glm::vec3 e = viz.halfExtents;
        glm::vec3 c[8];
        for (int i = 0; i < 8; ++i)
            c[i] = transformPoint(glm::vec3(
                (i & 1) ? e.x : -e.x, (i & 2) ? e.y : -e.y,
                (i & 4) ? e.z : -e.z));
        const int edges[12][2] = {{0,1},{1,3},{3,2},{2,0},{4,5},{5,7},
                                  {7,6},{6,4},{0,4},{1,5},{2,6},{3,7}};
        for (auto& edge : edges) line3D(c[edge[0]], c[edge[1]]);
    }
}

} // namespace saida
