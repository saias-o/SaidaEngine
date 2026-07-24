#pragma once

#include "scene/Node.hpp"
#include "core/Reflection.hpp"

#include <glm/glm.hpp>

#include <string>
#include <utility>

namespace saida {

enum class LightType {
    Directional,  // infinitely far; uses `direction`
    Point,        // local; uses the node's world position and `range`
    Spot,         // local; uses world position + `direction` + cone (`range`)
};

// How a light is meant to be evaluated. Everything is realtime today, but the
// field leaves room for future bake tooling without changing scene data.
enum class LightBakeMode {
    Realtime,  // always computed live
    Baked,     // contribution supplied by a baked GI/runtime cache
    Mixed,     // direct realtime + cached indirect
};

// A light source in the scene graph (cf. Godot Light3D / Unity Light).
class LightNode : public Node {
public:
    LightNode() : Node("LightNode"), type(LightType::Directional) {}
    explicit LightNode(std::string name, LightType type = LightType::Directional)
        : Node(std::move(name)), type(type) {}

    SAIDA_REFLECT_NODE(LightNode, "LightNode")

    LightNode* asLight() override { return this; }
    const LightNode* asLightConst() const override { return this; }

    LightType type;
    glm::vec3 color{1.0f};
    float intensity = 1.0f;

    // Directional only: light travel direction in the node's local space
    // (rotated by the node's world transform). Point lights ignore this and use
    // the node's world position instead.
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    float range = 10.0f;  // point and spot lights (attenuation falloff)

    // Spot only: cone half-angles in degrees. Light falls off smoothly between
    // the inner (full intensity) and outer (zero) cone.
    float spotInnerAngle = 25.0f;
    float spotOuterAngle = 35.0f;

    // Whether this light renders a shadow map. Only Directional and Spot honour
    // it (Point shadows would need a cube map — out of scope for now).
    bool castShadows = true;

    LightBakeMode bakeMode = LightBakeMode::Realtime;
};

} // namespace saida
