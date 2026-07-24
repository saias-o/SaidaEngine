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

    return 0;
}
