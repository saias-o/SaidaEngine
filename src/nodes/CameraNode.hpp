#pragma once

#include "scene/Node.hpp"

namespace saida {

// A camera placed in the scene (cf. Cinemachine 3 "CinemachineCamera"). Its world
// transform is the view pose; its lens drives the projection. The CameraDirector
// picks the active camera by `priority` and blends between cameras.
//
// A static camera needs no behaviour — just a pose and a priority. A moving
// camera (e.g. third-person follow) carries a behaviour that positions its node;
// the director only chooses and blends.
class CameraNode : public Node {
public:
    CameraNode();
    explicit CameraNode(std::string name);

    const char* typeName() const override { return "Camera"; }
    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;

    // Lens.
    float fovDegrees = 60.0f;  // vertical field of view
    float nearZ = 0.1f;
    float farZ = 100.0f;

    // Higher priority wins among active cameras (the director makes it "live").
    int priority = 0;
    // An inactive camera is ignored by the director (cf. enabling a vcam).
    bool active = true;
};

} // namespace saida
