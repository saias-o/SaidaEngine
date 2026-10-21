#include "WeatherRenderTestSupport.hpp"
#include "core/Paths.hpp"
#include "graphics/Material.hpp"
#include "graphics/ResourceManager.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/TerrainRingsNode.hpp"
#include "scene/Scene.hpp"

#include <array>
#include <string_view>

namespace {
using namespace saida;
using namespace saida::weather_test;

std::filesystem::path referenceShaders(const std::filesystem::path& output) {
    const auto root = std::filesystem::absolute(output / "fragment-reference");
    std::filesystem::create_directories(root / "shaders");
    std::filesystem::copy(shaderRoot(), root / "shaders",
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(SAIDA_TERRAIN_REFERENCE_VERTEX,
        root / "shaders/terrain_rings.vert.spv", std::filesystem::copy_options::overwrite_existing);
    return root;
}

SceneSetup terrain(bool wet, bool reversed, bool benchmark = false, bool relief = false) {
    return [=](Scene& scene, ResourceManager& resources) {
        auto& settings = scene.settings();
        settings.giEnabled = false; settings.iblEnabled = false;
        settings.bloomEnabled = false; settings.aoEnabled = false; settings.fogEnabled = false;
        settings.ambientLight = glm::vec4(.3f, .3f, .3f, 1);
        settings.clearColor = glm::vec4(.01f, .02f, .04f, 1);
        settings.rainIntensity = wet ? .7f : 0.f;
        settings.wetness = wet ? .8f : 0.f;
        auto* rings = scene.createChild<TerrainRingsNode>();
        rings->levels = benchmark ? 9 : 2;
        rings->baseSpacing = benchmark ? 16.f : 1.f;
        const uint8_t color[] = {90, 110, 70, 255, 130, 90, 50, 255,
                                 130, 90, 50, 255, 90, 110, 70, 255};
        MaterialDesc material;
        material.baseColor = glm::vec4(.6f, .7f, .5f, 1);
        material.albedoId = resources.registerGeneratedTexture(color, 2, 2, rhi::Format::RGBA8Srgb, true);
        material.rain.reception = 1.f;
        TerrainRingsNode::Layer land;
        land.material = resources.getMaterial(material);
        land.textureScale = .1f; land.macroVariation = .35f; land.macroNormalStrength = .3f;
        rings->setLayer(0, land);
        auto water = land; water.water = true;
        water.albedo = glm::vec3(.02f, .07f, .11f); water.roughness = .05f;
        rings->setLayer(1, water);
        for (int k = 0; k < rings->levels; ++k) {
            TerrainRingsNode::Level level;
            level.origin = rings->levelOrigin(k, {}); level.spacing = rings->levelSpacing(k);
            level.heights.resize(TerrainRingsNode::kSamples * TerrainRingsNode::kSamples);
            level.layers.resize(TerrainRingsNode::kResolution * TerrainRingsNode::kResolution);
            for (int z = 0; z < TerrainRingsNode::kSamples; ++z)
                for (int x = 0; x < TerrainRingsNode::kSamples; ++x) {
                    const auto p = level.origin + glm::dvec2(x, z) * level.spacing;
                    level.heights[z * TerrainRingsNode::kSamples + x] =
                        benchmark ? float(4.0 * std::sin(p.x * .004) * std::cos(p.y * .004)) :
                        relief ? float(.3 * std::sin(p.x * .1) * std::cos(p.y * .1)) : 0.f;
                    if (x < TerrainRingsNode::kResolution && z < TerrainRingsNode::kResolution)
                        level.layers[z * TerrainRingsNode::kResolution + x] = wet && z < 64 ? 1 : 0;
                }
            require(rings->setLevel(k, std::move(level)), "render fixture level accepted");
        }
        std::vector<TerrainRingsNode::Hole> holes;
        {
            // A skew hole crosses the morph band; its edges cut cells and pixels.
            TerrainRingsNode::Hole hole{{{48, -16}, {58, -16}, {60, 16}, {50, 16}}};
            if (benchmark) hole = {{{-500, -500}, {500, -500}, {500, 500}, {-500, 500}}};
            if (reversed) std::reverse(hole.begin(), hole.end());
            holes.push_back(hole);
        }
        rings->setHoles(std::move(holes));
        auto* sun = scene.createChild<LightNode>();
        sun->castShadows = false; sun->intensity = 2.f;
        sun->direction = glm::normalize(glm::vec3(.3f, -1.f, .2f));
    };
}
}

int terrainRingsRenderTests(int argc, char** argv) {
    using namespace saida;
    using namespace saida::weather_test;
    if (std::string_view(argv[1]) == "--benchmark") {
        require((argc == 7 || argc == 8) && std::string_view(argv[3]) == "--profile" &&
                std::string_view(argv[5]) == "--out", "benchmark: air|eye --profile trace.json --out image.png [--reference]");
        require(std::string_view(argv[2]) == "air" || std::string_view(argv[2]) == "eye", "benchmark viewpoint is air or eye");
        if (argc == 8) {
            require(std::string_view(argv[7]) == "--reference", "expected --reference");
            setRuntimeRoot(referenceShaders(std::filesystem::path(argv[6]).parent_path()).generic_string());
        }
        const bool air = std::string_view(argv[2]) == "air";
        Engine engine(terrain(false, false, true));
        engine.setCameraOverride(air ? glm::vec3(0, 500, 0) : glm::vec3(0, 1.8f, 0),
                                 air ? glm::vec3(0, 0, -200) : glm::vec3(0, 1.8f, -1000));
        engine.profileTo(argv[4]);
        CaptureRequest shot; shot.pngPath = argv[6]; shot.frame = 240; shot.fixedStep = 1.f / 60.f;
        engine.captureFrameThenExit(shot);
        engine.run();
        require(!engine.captureFailed(), "benchmark capture written");
    } else {
        require(std::string_view(argv[1]) == "--gpu", "expected --gpu [output-directory]");
        const std::filesystem::path output = argc > 2 ? argv[2] : "build/terrain-rings-captures";
        const auto originalRoot = runtimeRoot();
        const auto referenceRoot = referenceShaders(output);
        const std::array<glm::vec3, 3> eyes{{{50, 26, 24}, {30, 1.8f, 12}, {50, 900, 24}}};
        const std::array<glm::vec3, 3> targets{{{50, 0, 0}, {58, 0, 0}, {50, 0, 0}}};
        for (const bool wet : {false, true})
            for (const bool reversed : {false, true})
                for (size_t view = 0; view < eyes.size(); ++view) {
                const std::string name = std::string(wet ? "wet" : "dry") + (reversed ? "-cw" : "-ccw") + "-" + std::to_string(view);
                const auto full = capture(output / (name + "-culled.png"), terrain(wet, reversed, false, view == 1), eyes[view], targets[view]);
                bool visible = false;
                for (size_t p = 4; p < full.pixels.size(); p += 4)
                    visible |= !std::equal(full.pixels.begin(), full.pixels.begin() + 4, full.pixels.begin() + p);
                require(visible, "terrain fixture contains visible geometry, including the subpixel view");
                setRuntimeRoot(referenceRoot.generic_string());
                const auto reference = capture(output / (name + "-reference.png"), terrain(wet, reversed, false, view == 1), eyes[view], targets[view]);
                setRuntimeRoot(originalRoot);
                require(full.pixels == reference.pixels, "whole-cell culling preserves every pixel of fragment-only clipping, including morph/water/rain");
            }
        TerrainRingsNode* live = nullptr;
        const SceneSetup mutableTerrain = [&](Scene& scene, ResourceManager& resources) {
            terrain(false, false)(scene, resources);
            scene.refreshHierarchy();
            live = scene.terrainRings().front();
        };
        const auto clearHole = [&](unsigned frame) { if (frame == 2) live->setHoles({}); };
        const auto cleared = capture(output / "hole-removed-culled.png", mutableTerrain,
                                     eyes[0], targets[0], 1.25f, clearHole);
        setRuntimeRoot(referenceRoot.generic_string());
        const auto clearedReference = capture(output / "hole-removed-reference.png", mutableTerrain,
                                              eyes[0], targets[0], 1.25f, clearHole);
        setRuntimeRoot(originalRoot);
        require(cleared.pixels == clearedReference.pixels, "removing holes cannot reuse a stale frame-in-flight cell mask");
    }
    std::printf("[terrain-rings-render] OK (%d checks)\n", checks);
    return 0;
}
