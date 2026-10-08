#include "nodes/WaterNode.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool contains(const std::string& text, const char* value) {
    return text.find(value) != std::string::npos;
}

} // namespace

int main() {
    saida::WaterNode water;
    if (water.style != saida::WaterNode::Style::Realistic) return 1;
    if (water.cartoonColorSteps < 2.0f) return 2;
    if (water.cartoonCrestWidth <= 0.0f) return 3;

    const auto& type = saida::reflect::localDesc<saida::WaterNode>();
    if (!type.findProperty("style")) return 4;
    if (!type.findProperty("cartoonWaveScale")) return 5;
    if (!type.findProperty("cartoonShoreFrequency")) return 6;
    const auto* surface = type.findProperty("surface");
    if (!surface) return 16;
    for (const char* name : {"waveType", "waveIntensity", "windAngle", "gustStrength"}) {
        const auto* property = type.findProperty(name);
        if (!property) return 27;
        const nlohmann::json value = std::string(name) == "waveType" ? 2.0 : .5;
        property->set(&water,value);
        nlohmann::json readback;
        property->get(&water,readback);
        if (readback != value) return 28;
    }
    const nlohmann::json triangle = "0 2 0 1 2 0 0 2 1";
    surface->set(&water, triangle);
    nlohmann::json roundTrip;
    surface->get(&water, roundTrip);
    if (roundTrip != triangle) return 17;

    const auto shaderRoot = std::filesystem::path(SAIDA_PROJECT_ROOT) / "shaders";
    const std::string vertex = readText(shaderRoot / "cartoon_water.vert");
    const std::string fragment = readText(shaderRoot / "cartoon_water.frag");
    if (vertex.empty() || fragment.empty()) return 7;

    if (!contains(vertex, "vec3 position = vec3(xz.x, w.area.y, xz.y);")) return 8;
    if (contains(vertex, "sin(")) return 9;
    if (contains(fragment, "lighting.glsl")) return 10;
    if (contains(fragment, "fbm")) return 11;
    if (!contains(fragment, "waterDepthAt")) return 12;
    if (!contains(fragment, "shoreLine")) return 13;
    if (!contains(fragment, "cartoonWave")) return 14;
    if (!contains(fragment, "cartoonShore")) return 15;
    if (!contains(readText(shaderRoot / "water_surface.vert"), "fragWorldPos = base")) return 18;
    if (!contains(readText(shaderRoot / "cartoon_water_surface.vert"), "localToWorld")) return 19;

    // Realistic water must never read as tiles from the air: no wave train on
    // a commensurate heading lattice, every train filtered by what the mesh
    // and the pixel can carry, and a lattice hash that is exact (integer), so
    // a noise corner has one value whichever cell reaches it.
    const std::string waves = readText(shaderRoot / "water_wave.glsl");
    const std::string realistic = readText(shaderRoot / "water_shading.glsl");
    if (waves.empty() || realistic.empty()) return 20;
    if (contains(waves, "36.87")) return 21;
    if (!contains(waves, "waterTrainShown")) return 22;
    if (!contains(readText(shaderRoot / "water.vert"), "waterPixelFootprint")) return 23;
    if (!contains(realistic, "waterTrainShown")) return 24;
    if (!contains(readText(shaderRoot / "water_noise.glsl"), "#include \"noise.glsl\"") ||
        !contains(readText(shaderRoot / "noise.glsl"), "float hash21(ivec2")) return 25;
    if (!contains(realistic, "footprint")) return 26;

    // Realistic water reflects the sky as Fresnel says, is lit by the scene's
    // ambient even where diffuse IBL is off, and lets what lies beneath show
    // through unless asked to be opaque.
    if (water.reflectivity != 1.0f) return 29;
    if (!(water.transparency > 0.0f && water.transparency < 1.0f)) return 30;
    const auto* transparency = type.findProperty("transparency");
    if (!transparency) return 31;
    transparency->set(&water, nlohmann::json(0.0));
    if (water.transparency != 0.0f) return 32;
    if (!contains(realistic, "vec3 ambient = lights.ambient.rgb;")) return 33;
    if (!contains(realistic, "w.optics.x")) return 34;
    if (!contains(readText(shaderRoot / "water_types.glsl"), "vec4 optics;")) return 35;
    if (!contains(readText(shaderRoot / "water_distant.glsl"), "w.optics = vec4(0);")) return 36;

    return 0;
}
