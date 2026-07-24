#pragma once

#include "scene/NodeId.hpp"
#include "core/Easing.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace saida {

class Scene;
class Camera;

// The camera "brain" (cf. Cinemachine 3 CinemachineBrain). Each frame it picks the
// active CameraNode (highest `priority` among active cameras) and drives the engine
// Camera from its pose + lens. When the live camera changes, it blends smoothly
// (lerp position, slerp rotation, lerp fov) over `blendDuration`.
//
// Stateful only across frames for the blend; owns no scene objects. The Engine
// runs it during Play, after the scene update.
class CameraDirector {
public:
    // Returns true if a camera was found and drove `out`; false if the scene has no
    // active CameraNode (the caller keeps its own camera, e.g. the editor fly cam).
    bool update(Scene& scene, Camera& out, float dt);

    // Forget blend state (call on Play start so the first camera snaps in).
    void reset();

    float blendDuration = 0.6f;          // seconds
    Easing blendEasing = Easing::InOutQuad;

private:
    struct State {
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        float fov = 60.0f;
        float nearZ = 0.1f;
        float farZ = 1000.0f;
    };

    static void applyTo(const State& s, Camera& out);

    State current_;            // the live (possibly blended) output
    bool hasCurrent_ = false;

    State blendFrom_;          // snapshot at the moment the live camera changed
    bool blending_ = false;
    float blendT_ = 0.0f;      // seconds elapsed in the current blend

    NodeId liveId_ = kNodeInvalid;  // which camera is currently live (change → blend)
};

} // namespace saida
