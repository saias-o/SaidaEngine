#pragma once

#include "scene/Node.hpp"
#include "core/Reflection.hpp"

#include <glm/glm.hpp>

namespace saida {

// Reflected data for the dedicated procedural water pipeline.
// The node position is the plane center; `size` is its half-extent in metres.
class WaterNode : public Node {
public:
    WaterNode() : Node("Water") {}

    SAIDA_REFLECT_NODE(WaterNode, "Water")

    enum class Style { Realistic = 0, Cartoon = 1 };
    // Analytic shore shape used by the water shaders.
    enum class ShoreMode { None = 0, Beach = 1, Lake = 2 };

    Style style = Style::Realistic;
    float size = 300.0f;          // half-extent of the plane (m)

    glm::vec3 deepColor{0.01f, 0.07f, 0.11f};   // refracted body tint
    glm::vec3 foamColor{0.80f, 0.90f, 0.96f};   // crest foam colour
    float roughness = 0.05f;       // reflection sharpness (low = mirror-like)
    float reflectivity = 0.55f;    // strength of the Fresnel sky reflection

    // Big rolling waves (vertex displacement).
    float amplitude = 0.25f;       // wave height (m)
    float wavelength = 15.0f;      // distance between primary crests (m)
    float waveSpeed = 0.7f;        // scroll speed (kept from before)
    float choppiness = 0.25f;      // crest sharpening (Gerstner-like horizontal pull)

    // Two procedural normal layers.
    float detailScale = 0.7f;      // layer 1: feature frequency (×world metres)
    float detailSpeed = 0.06f;     // layer 1: flow speed (kept from before)
    float detailStrength = 0.2f;   // layer 1: ripple intensity
    float detailAngle = 100.0f;    // layer 1: flow direction (deg)
    float detail2Scale = 0.9f;     // layer 2: finer features
    float detail2Speed = 0.12f;    // layer 2: flow speed (kept from before)
    float detail2Strength = 0.35f; // layer 2: ripple intensity
    float detail2Angle = 110.0f;   // layer 2: flow direction (deg)

    // Look / shading.
    float fresnelPower = 5.0f;     // higher = reflection only at grazing angles
    float specularPower = 35.0f;   // sun sparkle tightness
    float specularIntensity = 0.25f;
    float foamThreshold = 0.2f;    // crest height where foam starts (0..1)
    float foamIntensity = 0.06f;   // how white the foam gets

    float warpAmount = 0.1f;       // domain-warp that breaks large-scale regularity
    float detailFadeDistance = 200.0f;  // ripples calm down past this (anti-shimmer)

    // Shore (beach / lake).
    ShoreMode shoreMode = ShoreMode::None;

    glm::vec3 shallowColor{0.20f, 0.58f, 0.62f};  // water tint at the waterline (sandy turquoise)
    float depthColorFalloff = 6.0f;  // metres of depth to reach the full deep colour
    float edgeFade = 0.6f;           // metres of depth over which the edge fades from sand to water
    float shoreSlope = 0.10f;        // seabed steepness: depth gained per metre away from the waterline

    // Beach mode: `shoreAngle` is inland direction; `shoreWaterline` offsets it.
    float shoreAngle = 0.0f;
    float shoreWaterline = 0.0f;

    // Lake mode: radius of the pond centred on the node.
    float lakeRadius = 50.0f;

    // Swash (wave run-up at the shoreline) and foam.
    float shoreFoam = 0.8f;        // shoreline foam intensity (0 = none)
    float foamWidth = 0.5f;        // base width of the wet foam band (depth metres)
    float swashSpeed = 1.2f;       // how fast the wash runs up and back
    float swashAmount = 0.4f;      // extra run-up reach added by each wash (depth metres)
    float waveFlatten = 1.5f;      // waves flatten below this depth (m) so they don't clip the sand

    float cartoonWaveScale = 0.22f;
    float cartoonWaveSpeed = 1.0f;
    float cartoonWaveAngle = 20.0f;
    float cartoonWaveSharpness = 0.65f;

    float cartoonDetailScale = 0.55f;
    float cartoonDetailSpeed = 0.45f;
    float cartoonDetailAngle = 115.0f;
    float cartoonDetailStrength = 0.35f;

    float cartoonColorSteps = 4.0f;
    float cartoonColorContrast = 0.28f;
    float cartoonCrestWidth = 0.10f;
    float cartoonCrestIntensity = 0.65f;

    float cartoonShoreFrequency = 0.16f;
    float cartoonShoreIrregularity = 0.25f;
    float cartoonShoreSharpness = 0.10f;
    float cartoonShoreBands = 2.0f;
};

} // namespace saida
