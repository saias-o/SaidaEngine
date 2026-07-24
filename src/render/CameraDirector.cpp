#include "render/CameraDirector.hpp"

#include "core/Camera.hpp"
#include "nodes/CameraNode.hpp"
#include "scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace saida {

namespace {
// Decompose a world matrix into position + (scale-free) rotation. Cameras are
// normally unscaled; normalizing the basis keeps the quaternion valid regardless.
void poseFromWorld(const glm::mat4& world, glm::vec3& pos, glm::quat& rot) {
    pos = glm::vec3(world[3]);
    glm::mat3 basis(world);
    if (glm::length(basis[0]) > 1e-6f) basis[0] = glm::normalize(basis[0]);
    if (glm::length(basis[1]) > 1e-6f) basis[1] = glm::normalize(basis[1]);
    if (glm::length(basis[2]) > 1e-6f) basis[2] = glm::normalize(basis[2]);
    rot = glm::normalize(glm::quat_cast(basis));
}
}  // namespace

void CameraDirector::reset() {
    hasCurrent_ = false;
    blending_ = false;
    blendT_ = 0.0f;
    liveId_ = kNodeInvalid;
}

bool CameraDirector::update(Scene& scene, Camera& out, float dt) {
    // Pick the active camera with the highest priority (ties: first encountered).
    CameraNode* live = nullptr;
    glm::mat4 liveWorld(1.0f);
    int bestPriority = 0;
    scene.traverse([&](Node& n, const glm::mat4& world) {
        auto* cam = dynamic_cast<CameraNode*>(&n);
        if (!cam || !cam->active || !cam->isActiveInHierarchy()) return;
        if (!live || cam->priority > bestPriority) {
            live = cam;
            liveWorld = world;
            bestPriority = cam->priority;
        }
    });

    if (!live) {
        reset();  // no camera → don't drive `out`; next mount blends fresh
        return false;
    }

    // Target pose/lens from the live camera's world transform.
    State target;
    poseFromWorld(liveWorld, target.position, target.rotation);
    target.fov = live->fovDegrees;
    target.nearZ = live->nearZ;
    target.farZ = live->farZ;

    if (!hasCurrent_) {
        // First camera: snap (no blend from nothing).
        current_ = target;
        hasCurrent_ = true;
        blending_ = false;
        liveId_ = live->id();
    } else if (live->id() != liveId_) {
        // Live camera changed: blend from wherever we currently are.
        blendFrom_ = current_;
        blending_ = true;
        blendT_ = 0.0f;
        liveId_ = live->id();
    }

    if (blending_) {
        blendT_ += dt;
        float t = blendDuration > 0.0f ? blendT_ / blendDuration : 1.0f;
        if (t >= 1.0f) { t = 1.0f; blending_ = false; }
        float e = applyEasing(blendEasing, t);
        current_.position = glm::mix(blendFrom_.position, target.position, e);
        current_.rotation = glm::slerp(blendFrom_.rotation, target.rotation, e);
        current_.fov = glm::mix(blendFrom_.fov, target.fov, e);
        current_.nearZ = glm::mix(blendFrom_.nearZ, target.nearZ, e);
        current_.farZ = glm::mix(blendFrom_.farZ, target.farZ, e);
    } else {
        // Track the live camera directly (it may be moving, e.g. a follow cam).
        current_ = target;
    }

    applyTo(current_, out);
    return true;
}

void CameraDirector::applyTo(const State& s, Camera& out) {
    out.position = s.position;
    out.fovDegrees = s.fov;
    out.nearZ = s.nearZ;
    out.farZ = s.farZ;
    // Camera is yaw/pitch (no roll): point it along the state's forward (-Z).
    glm::vec3 forward = s.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    out.lookAt(s.position + forward);
}

} // namespace saida
