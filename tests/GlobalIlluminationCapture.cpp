#include "Engine.hpp"
#include "core/Log.hpp"
#include "graphics/Material.hpp"
#include "graphics/ResourceManager.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/MeshNode.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSettingsSerialization.hpp"

#include <glm/gtc/quaternion.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

// GPU fixture driver. Assertions live in verify_global_illumination.py so one
// failure never prevents the remaining contracts from producing their evidence.
namespace {
using namespace saida;
constexpr glm::vec3 kEye{0.f, 2.f, 7.f};
constexpr glm::vec3 kTarget{0.f, 1.f, 0.f};

struct Fixture {
    std::string name;
    bool gi = false;
    bool optOut = false;
    float travel = 0.f;
    bool rebase = false;
    float rotation = 0.f;
    std::string sky;
    unsigned frames = 240;
    unsigned motionFrame = 120;
    std::filesystem::path image, profile;
};

MeshNode* box(Scene& scene, ResourceManager& resources, const char* name,
              glm::vec3 position, glm::vec3 scale, glm::vec3 albedo,
              bool metal = false, bool optOut = false) {
    // Exercise the real material reader; a missing future opt-out must fail
    // its pixels rather than making this phase-0 harness require that API.
    nlohmann::json node{{"type", "MeshNode"}, {"name", name}, {"mesh", "cube"},
        {"baseColor", {albedo.r, albedo.g, albedo.b, 1.f}},
        {"metallic", metal ? 1.f : 0.f}, {"roughness", 1.f},
        {"castShadows", true}, {"receiveGlobalIllumination", !optOut},
        {"transform", {{"position", {position.x, position.y, position.z}},
                       {"scale", {scale.x, scale.y, scale.z}}}}};
    auto* result = scene.createChild<MeshNode>();
    result->deserialize(node, resources);
    return result;
}

void setup(Scene& scene, ResourceManager& resources, const Fixture& f) {
    auto& settings = scene.settings();
    settings.giEnabled = f.gi;
    settings.giMode = GIMode::FullRealtime;
    settings.giIntensity = 1.f;
    settings.aoEnabled = false;
    settings.iblEnabled = false;
    settings.bloomEnabled = false;
    settings.fogEnabled = false;
    settings.ambientLight = glm::vec4(.2f, .2f, .2f, 1.f);
    settings.clearColor = glm::vec4(0.f, 0.f, 0.f, 1.f);
    if (!f.sky.empty()) {
        settings.iblEnabled = true;
        settings.iblDiffuseIntensity = 1.f;
        settings.iblSpecularIntensity = 0.f;
        settings.ambientLight = glm::vec4(0, 0, 0, 1);
        settings.skyboxTexture = resources.getOrRegister(f.sky, AssetType::Texture);
    }
    // The serializer will consume this once Follow exists. Ignoring it today
    // is observable in the scrolling contract, not disguised as a skip.
    if (f.travel != 0.f || f.rebase)
        applySceneSettings(nlohmann::json{{"giVolume", "Follow"}}, settings, resources);

    box(scene, resources, "Floor", {0, -.4f, 0}, {18, .8f, 18}, {.2f, .2f, .2f},
        f.name == "metal", f.optOut);
    if (f.name == "open" || f.name == "metal") return;

    if (f.name == "room") {
        settings.ambientLight = glm::vec4(.4f, .4f, .4f, 1.f);
        box(scene, resources, "Back", {0, 2, -3}, {8, 4, 1}, glm::vec3(.2f));
        box(scene, resources, "Left", {-4, 2, 0}, {1, 4, 7}, glm::vec3(.2f));
        box(scene, resources, "Right", {4, 2, 0}, {1, 4, 7}, glm::vec3(.2f));
        box(scene, resources, "Roof", {0, 4, 0}, {9, 1, 7}, glm::vec3(.2f));
        // Front wall with a one-metre doorway at the left edge. The camera
        // stays inside; the floor sample is far from the opening.
        box(scene, resources, "Front", {.6f, 2, 3}, {6.8f, 4, 1}, glm::vec3(.2f));
        box(scene, resources, "Door jamb", {-3.8f, 2, 3}, {.4f, 4, 1}, glm::vec3(.2f));
        box(scene, resources, "Lintel", {-3.2f, 3.4f, 3}, {1, 1.2f, 1}, glm::vec3(.2f));
    } else {
        settings.ambientLight = glm::vec4(.025f, .025f, .025f, 1.f);
        box(scene, resources, "Receiver", {-2.5f, 2, 0}, {1, 4, 12}, glm::vec3(.2f), false, f.optOut);
        const glm::vec3 colour = f.name == "canyon-blue" ? glm::vec3(.04f, .04f, .3f)
                                                                      : glm::vec3(.3f, .04f, .04f);
        box(scene, resources, "Reflector", {2.5f, 2, 0}, {1, 4, 12}, colour);
    }
    auto* sun = scene.createChild<LightNode>();
    sun->direction = glm::normalize(glm::vec3(1.f, -.5f, .05f));
    sun->intensity = 3.f;
    sun->castShadows = true;
}

Fixture parse(int argc, char** argv) {
    if (argc != 2) throw std::invalid_argument("usage: saida_gi_capture fixture.json");
    std::ifstream input(argv[1]);
    const auto j = nlohmann::json::parse(input);
    Fixture f;
    f.name = j.at("scene").get<std::string>();
    if (f.name != "open" && f.name != "metal" && f.name != "room" &&
        f.name != "canyon-red" && f.name != "canyon-blue")
        throw std::invalid_argument("unknown GI fixture scene");
    f.gi = j.at("gi").get<bool>();
    f.optOut = j.value("optOut", false);
    f.travel = j.value("travel", 0.f);
    f.rebase = j.value("rebase", false);
    f.rotation = j.value("rotation", 0.f);
    f.sky = j.value("sky", std::string{});
    f.frames = j.value("frames", 240u);
    f.motionFrame = j.value("motionFrame", 120u);
    f.image = j.at("image").get<std::string>();
    f.profile = j.value("profile", std::string{});
    if (f.frames == 0 || !std::isfinite(f.travel) || !std::isfinite(f.rotation))
        throw std::invalid_argument("invalid GI capture parameters");
    return f;
}
}

int main(int argc, char** argv) {
    try {
        const auto f = parse(argc, argv);
        const char* hidden = std::getenv("SAIDA_WINDOW_HIDDEN");
        if (!hidden || std::string(hidden) != "1")
            throw std::invalid_argument("GPU fixture requires SAIDA_WINDOW_HIDDEN=1");
        Engine engine([&](Scene& scene, ResourceManager& resources) { setup(scene, resources, f); });
        const glm::vec3 eye = f.name == "room" ? glm::vec3(0, 1.8f, 1.5f) : kEye;
        const glm::vec3 target = f.name == "room" ? glm::vec3(0, .2f, -1) : kTarget;
        engine.setCameraOverride(eye, target);
        engine.setCameraFovOverride(60.f);
        engine.setOnFrame([&, frame = 0u](float) mutable {
            if (++frame != f.motionFrame) return;
            const glm::vec3 shift(f.travel, 0, 0);
            if (f.rebase) {
                const auto q = glm::angleAxis(glm::radians(f.rotation), glm::vec3(0, 1, 0));
                engine.scene().rebaseOrigin(shift, q);
                engine.setCameraOverride(q * eye + shift, q * target + shift);
            } else if (f.travel != 0.f) {
                // Move the fixture with the eye to keep its expected lighting
                // constant while the camera crosses the old fixed GI volume.
                for (const auto& child : engine.scene().children())
                    child->transform().position += shift;
                engine.setCameraOverride(eye + shift, target + shift);
            }
        });
        if (!f.profile.empty()) engine.profileTo(f.profile.generic_string());
        CaptureRequest shot;
        shot.pngPath = f.image.generic_string();
        shot.frame = f.frames;
        shot.fixedStep = 1.f / 60.f;
        // All fixture geometry/materials are synchronous built-ins. No settle
        // frames may shift the scripted motion away from its requested frame.
        shot.waitForAssets = !f.sky.empty();
        engine.captureFrameThenExit(shot);
        engine.run();
        return engine.captureFailed() ? EXIT_FAILURE : EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "GI fixture: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
