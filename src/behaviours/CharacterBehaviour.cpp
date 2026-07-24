#include "behaviours/CharacterBehaviour.hpp"
#include "scene/Node.hpp"
#include "scene/SceneTree.hpp"
#include "physics/CharacterBodyNode.hpp"
#include "scene/animation/AnimGraphAsset.hpp"
#include "scene/animation/Animator.hpp"
#include "graphics/ResourceManager.hpp"
#include "core/Input.hpp"
#include "core/Log.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace saida {

namespace {
constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};

// Horizontal forward/right of the active camera (found by group), so movement is
// camera-relative. Falls back to world axes when no camera is present.
void cameraBasis(SceneTree* tree, glm::vec3& forward, glm::vec3& right) {
    forward = glm::vec3(0.0f, 0.0f, -1.0f);
    right = glm::vec3(1.0f, 0.0f, 0.0f);
    if (!tree) return;
    Node* cam = tree->firstInGroup("camera");
    if (!cam) return;
    glm::vec3 camFwd = glm::vec3(cam->worldTransform() * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
    camFwd.y = 0.0f;
    if (glm::dot(camFwd, camFwd) < 1e-6f) return;  // camera looking straight up/down
    forward = glm::normalize(camFwd);
    right = glm::normalize(glm::cross(forward, kWorldUp));
}
}  // namespace

void CharacterBehaviour::onReady() {
    // Default WASD/ZQSD bindings (GLFW key codes are physical QWERTY positions, so
    // these positions already cover ZQSD on an AZERTY layout). Arrow keys too.
    // Bindings are additive (max strength wins), so multiple keys per action work.
    Input::bindKey("MoveForward", KeyCode::W);
    Input::bindKey("MoveLeft", KeyCode::A);
    Input::bindKey("MoveBackward", KeyCode::S);
    Input::bindKey("MoveRight", KeyCode::D);
    Input::bindKey("MoveForward", KeyCode::Up);
    Input::bindKey("MoveLeft", KeyCode::Left);
    Input::bindKey("MoveBackward", KeyCode::Down);
    Input::bindKey("MoveRight", KeyCode::Right);
    Input::bindKey("Jump", KeyCode::Space);
    Input::bindKey("Sprint", KeyCode::LeftShift);
}

void CharacterBehaviour::onUpdate(float dt) {
    CharacterBodyNode* body = node() ? node()->asCharacterBody() : nullptr;
    if (!body) {
        if (!warned_) {
            Log::warn("CharacterBehaviour must be on a CharacterBody node — ignored");
            warned_ = true;
        }
        return;
    }

    // Logic only: read input → write velocity. The engine performs the slide in
    // the physics step (no moveAndSlide to call or forget).
    glm::vec2 in = Input::getVector("MoveLeft", "MoveRight", "MoveBackward", "MoveForward");

    // Movement is relative to the active camera's horizontal facing.
    glm::vec3 forward, right;
    cameraBasis(tree(), forward, right);
    glm::vec3 moveDir = right * in.x + forward * in.y;
    if (glm::length(moveDir) > 1.0f) moveDir = glm::normalize(moveDir);  // no faster diagonals

    float speed = moveSpeed * (Input::isActionHeld("Sprint") ? sprintMultiplier : 1.0f);
    glm::vec3 v = body->velocity;
    v.x = moveDir.x * speed;
    v.z = moveDir.z * speed;

    if (body->isOnFloor()) {
        v.y = 0.0f;
        if (Input::isActionJustPressed("Jump")) v.y = jumpForce;
    } else {
        v.y -= gravity * dt;  // gravity while airborne
    }

    body->velocity = v;

    bool moving = glm::length(glm::vec2(v.x, v.z)) > 0.1f;

    // Turn to face the movement direction (smoothed). The model's forward is -Z.
    if (faceMovement && moving) {
        glm::vec3 flatDir = glm::normalize(glm::vec3(moveDir.x, 0.0f, moveDir.z));
        glm::quat targetRot =
            glm::quat_cast(glm::inverse(glm::lookAt(glm::vec3(0.0f), flatDir, kWorldUp)));
        float a = 1.0f - std::exp(-turnSpeed * dt);
        node()->transform().rotation =
            glm::normalize(glm::slerp(node()->transform().rotation, targetRot, a));
    }

    updateAnimation(body->isOnFloor(), moving);
}

void CharacterBehaviour::updateAnimation(bool onFloor, bool moving) {
    if (!animator_) animator_ = node()->findBehaviourInChildren<Animator>();
    if (!animator_) return;  // no skinned character → nothing to drive

    if (!graph.empty() && !graphFailed_) {
        if (!graphApplied_) {
            ResourceManager& resources = tree()->resources();
            if (graphAssetId_ == kAssetInvalid) {
                graphAssetId_ = resources.loadAnimGraph(tree()->resolveProjectPath(graph));
                if (graphAssetId_ == kAssetInvalid) {
                    graphFailed_ = true;
                    Log::warn("Character: cannot request anim graph '", graph, "'");
                    return;
                }
            }

            const AssetLoadState state = resources.animGraphLoadState(graphAssetId_);
            if (state == AssetLoadState::Queued || state == AssetLoadState::Loading)
                return;
            if (state == AssetLoadState::Failed) {
                graphFailed_ = true;
                const std::string error = resources.animGraphLoadError(graphAssetId_);
                Log::warn("Character: cannot apply anim graph '", graph, "'",
                          error.empty() ? "" : (": " + error));
                return;
            }

            const AnimGraphAsset* loaded = resources.getAnimGraph(graphAssetId_);
            std::vector<AssetDiagnostic> diags;
            if (!loaded || !animator_->setGraph(*loaded, &diags)) {
                graphFailed_ = true;
                Log::warn("Character: cannot apply anim graph '", graph, "'",
                          diags.empty() ? "" : (": " + diags.front().message));
                return;
            }
            graphApplied_ = true;
        }
        animator_->setFloat("speed", moving ? 1.0f : 0.0f);
        return;
    }

    const std::string& want = !onFloor ? jumpClip : (moving ? walkClip : idleClip);
    if (!want.empty() && animator_->clips().count(want))
        animator_->play(want);  // play() no-ops if it's already the current clip
}

void CharacterBehaviour::describe(reflect::TypeBuilder<CharacterBehaviour>& t) {
    t.doc("Third-person character controller: WASD relative to the active camera, "
          "jump, and movement-driven animation. Attach to a CharacterBody node.");
    t.property("moveSpeed", &CharacterBehaviour::moveSpeed).range(0.0, 50.0);
    t.property("sprintMultiplier", &CharacterBehaviour::sprintMultiplier).range(1.0, 5.0)
        .tooltip("speed factor while holding Sprint (Shift)");
    t.property("jumpForce", &CharacterBehaviour::jumpForce).range(0.0, 30.0);
    t.property("gravity", &CharacterBehaviour::gravity).range(0.0, 50.0);
    t.property("faceMovement", &CharacterBehaviour::faceMovement)
        .tooltip("turn to face the movement direction");
    t.property("turnSpeed", &CharacterBehaviour::turnSpeed).range(0.0, 50.0);
    t.property("idleClip", &CharacterBehaviour::idleClip).tooltip("animation clip name");
    t.property("walkClip", &CharacterBehaviour::walkClip).tooltip("animation clip name");
    t.property("jumpClip", &CharacterBehaviour::jumpClip).tooltip("animation clip name");
    t.property("graph", &CharacterBehaviour::graph)
        .tooltip(".sgraph path (project-relative); when set, drives the 'speed' parameter");
}

} // namespace saida
