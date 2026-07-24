#include "nodes/WaterNode.hpp"

namespace saida {

void WaterNode::describe(reflect::TypeBuilder<WaterNode>& t) {
    t.doc("Parametric water plane with realistic and flat cartoon styles.");
    t.property("style", &WaterNode::style)
        .enumValues({"Realistic", "Cartoon"});
    t.property("size", &WaterNode::size).range(1.0, 5000.0).tooltip("half-extent of the plane (m)");
    t.property("deepColor", &WaterNode::deepColor).tooltip("refracted body tint");
    t.property("foamColor", &WaterNode::foamColor).tooltip("wave-crest foam colour");
    t.property("roughness", &WaterNode::roughness).range(0.02, 0.4)
        .tooltip("reflection sharpness (low = mirror-like)");
    t.property("reflectivity", &WaterNode::reflectivity).range(0.0, 2.0)
        .tooltip("Fresnel sky-reflection strength");

    t.property("amplitude", &WaterNode::amplitude).range(0.0, 5.0).tooltip("wave height (m)");
    t.property("wavelength", &WaterNode::wavelength).range(0.5, 200.0)
        .tooltip("distance between primary crests (m)");
    t.property("waveSpeed", &WaterNode::waveSpeed).range(0.0, 10.0);
    t.property("choppiness", &WaterNode::choppiness).range(0.0, 1.5)
        .tooltip("crest sharpening (horizontal pull)");

    t.property("detailScale", &WaterNode::detailScale).range(0.02, 5.0)
        .tooltip("normal layer 1: feature frequency");
    t.property("detailSpeed", &WaterNode::detailSpeed).range(0.0, 2.0).tooltip("layer 1 flow speed");
    t.property("detailStrength", &WaterNode::detailStrength).range(0.0, 3.0).tooltip("layer 1 intensity");
    t.property("detailAngle", &WaterNode::detailAngle).range(0.0, 360.0).tooltip("layer 1 flow direction (deg)");
    t.property("detail2Scale", &WaterNode::detail2Scale).range(0.02, 5.0)
        .tooltip("normal layer 2: feature frequency");
    t.property("detail2Speed", &WaterNode::detail2Speed).range(0.0, 2.0).tooltip("layer 2 flow speed");
    t.property("detail2Strength", &WaterNode::detail2Strength).range(0.0, 3.0).tooltip("layer 2 intensity");
    t.property("detail2Angle", &WaterNode::detail2Angle).range(0.0, 360.0).tooltip("layer 2 flow direction (deg)");

    t.property("fresnelPower", &WaterNode::fresnelPower).range(1.0, 8.0)
        .tooltip("higher = reflection only at grazing angles");
    t.property("specularPower", &WaterNode::specularPower).range(8.0, 2000.0)
        .tooltip("sun sparkle tightness");
    t.property("specularIntensity", &WaterNode::specularIntensity).range(0.0, 5.0);
    t.property("foamThreshold", &WaterNode::foamThreshold).range(0.0, 1.0)
        .tooltip("crest height where foam starts");
    t.property("foamIntensity", &WaterNode::foamIntensity).range(0.0, 1.0);
    t.property("warpAmount", &WaterNode::warpAmount).range(0.0, 6.0)
        .tooltip("domain warp that breaks large-scale tiling");
    t.property("detailFadeDistance", &WaterNode::detailFadeDistance).range(10.0, 2000.0)
        .tooltip("ripples calm down past this distance");

    t.property("shoreMode", &WaterNode::shoreMode)
        .enumValues({"None", "Beach", "Lake"})
        .tooltip("None = endless water; Beach = straight shoreline; Lake = round pond");
    t.property("shallowColor", &WaterNode::shallowColor)
        .tooltip("water tint at the waterline (deepens to deepColor)");
    t.property("depthColorFalloff", &WaterNode::depthColorFalloff).range(0.1, 50.0)
        .tooltip("depth (m) to reach the full deep colour");
    t.property("edgeFade", &WaterNode::edgeFade).range(0.02, 5.0)
        .tooltip("depth (m) over which the shoreline edge fades in");
    t.property("shoreSlope", &WaterNode::shoreSlope).range(0.005, 2.0)
        .tooltip("seabed steepness: depth gained per metre from the waterline");
    t.property("shoreAngle", &WaterNode::shoreAngle).range(0.0, 360.0)
        .tooltip("beach: inland direction (deg)");
    t.property("shoreWaterline", &WaterNode::shoreWaterline).range(-5000.0, 5000.0)
        .tooltip("beach: waterline distance from the node centre (m)");
    t.property("lakeRadius", &WaterNode::lakeRadius).range(1.0, 5000.0)
        .tooltip("lake: water radius from the node centre (m)");
    t.property("shoreFoam", &WaterNode::shoreFoam).range(0.0, 1.0)
        .tooltip("shoreline foam intensity");
    t.property("foamWidth", &WaterNode::foamWidth).range(0.0, 5.0)
        .tooltip("base width of the wet foam band (depth m)");
    t.property("swashSpeed", &WaterNode::swashSpeed).range(0.0, 10.0)
        .tooltip("how fast the wash runs up and back");
    t.property("swashAmount", &WaterNode::swashAmount).range(0.0, 5.0)
        .tooltip("extra run-up reach per wash (depth m)");
    t.property("waveFlatten", &WaterNode::waveFlatten).range(0.05, 10.0)
        .tooltip("waves flatten below this depth so they don't clip the sand");

    t.property("cartoonWaveScale", &WaterNode::cartoonWaveScale).range(0.01, 4.0);
    t.property("cartoonWaveSpeed", &WaterNode::cartoonWaveSpeed).range(-10.0, 10.0);
    t.property("cartoonWaveAngle", &WaterNode::cartoonWaveAngle).range(0.0, 360.0);
    t.property("cartoonWaveSharpness", &WaterNode::cartoonWaveSharpness).range(0.0, 0.98);
    t.property("cartoonDetailScale", &WaterNode::cartoonDetailScale).range(0.01, 8.0);
    t.property("cartoonDetailSpeed", &WaterNode::cartoonDetailSpeed).range(-10.0, 10.0);
    t.property("cartoonDetailAngle", &WaterNode::cartoonDetailAngle).range(0.0, 360.0);
    t.property("cartoonDetailStrength", &WaterNode::cartoonDetailStrength).range(0.0, 1.0);
    t.property("cartoonColorSteps", &WaterNode::cartoonColorSteps).range(1.0, 12.0);
    t.property("cartoonColorContrast", &WaterNode::cartoonColorContrast).range(0.0, 1.0);
    t.property("cartoonCrestWidth", &WaterNode::cartoonCrestWidth).range(0.0, 0.5);
    t.property("cartoonCrestIntensity", &WaterNode::cartoonCrestIntensity).range(0.0, 1.0);
    t.property("cartoonShoreFrequency", &WaterNode::cartoonShoreFrequency).range(0.0, 4.0);
    t.property("cartoonShoreIrregularity", &WaterNode::cartoonShoreIrregularity).range(0.0, 2.0);
    t.property("cartoonShoreSharpness", &WaterNode::cartoonShoreSharpness).range(0.005, 1.0);
    t.property("cartoonShoreBands", &WaterNode::cartoonShoreBands).range(1.0, 4.0);
}

} // namespace saida
