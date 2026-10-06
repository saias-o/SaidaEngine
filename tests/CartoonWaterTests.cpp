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
    if (!contains(readText(shaderRoot / "water_surface.vert"), "waterWaveAt")) return 18;
    if (!contains(readText(shaderRoot / "cartoon_water_surface.vert"), "localToWorld")) return 19;

    // Realistic water must never read as tiles from the air: no wave train on
    // a commensurate heading lattice, every train filtered by what the mesh
    // and the pixel can carry, and a lattice hash that is exact (integer), so
    // a noise corner has one value whichever cell reaches it.
    const std::string waves = readText(shaderRoot / "water_wave.glsl");
    const std::string realistic = readText(shaderRoot / "water.frag");
    if (waves.empty() || realistic.empty()) return 20;
    if (contains(waves, "36.87")) return 21;
    if (!contains(waves, "waterTrainShown")) return 22;
    if (!contains(readText(shaderRoot / "water.vert"), "waterPixelFootprint")) return 23;
    if (!contains(realistic, "waterTrainShown")) return 24;
    if (!contains(realistic, "float hash21(ivec2")) return 25;
    if (!contains(realistic, "footprint")) return 26;

    return 0;
}
