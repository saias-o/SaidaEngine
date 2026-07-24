#include "nodes/LightNode.hpp"

namespace saida {

void LightNode::describe(reflect::TypeBuilder<LightNode>& t) {
    t.doc("A light source in the scene graph (Directional / Point / Spot).");
    t.property("lightType", &LightNode::type)
        .enumValues({"Directional", "Point", "Spot"});
    t.property("color", &LightNode::color).tooltip("linear RGB");
    t.property("intensity", &LightNode::intensity).range(0.0, 100.0);
    t.property("direction", &LightNode::direction)
        .tooltip("local light travel direction (Directional/Spot)");
    t.property("range", &LightNode::range).range(0.0, 1000.0)
        .tooltip("attenuation falloff distance (Point/Spot)");
    t.property("spotInnerAngle", &LightNode::spotInnerAngle).range(0.0, 90.0)
        .tooltip("Spot cone inner half-angle (deg)");
    t.property("spotOuterAngle", &LightNode::spotOuterAngle).range(0.0, 90.0)
        .tooltip("Spot cone outer half-angle (deg)");
    t.property("castShadows", &LightNode::castShadows)
        .tooltip("Directional/Spot only");
    t.property("bakeMode", &LightNode::bakeMode)
        .enumValues({"Realtime", "Baked", "Mixed"});
}

} // namespace saida
