#include "Engine.hpp"
#include "behaviours/LODGroupBehaviour.hpp"
#include "render/gi/GIContributors.hpp"
#include "core/Window.hpp"
#include "graphics/ResourceManager.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/MeshNode.hpp"
#include "scene/Scene.hpp"

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// GPU fixture driver. Assertions live in verify_global_illumination.py so one
// failure never prevents the remaining contracts from producing their evidence.
namespace {
using namespace saida;
constexpr glm::vec3 kEye{0.f, 2.f, 7.f};
constexpr glm::vec3 kTarget{0.f, 1.f, 0.f};

struct Fixture {
    std::string name;
    bool gi = false;
    GIQuality quality = GIQuality::Auto;
    bool optOut = false;
    bool follow = false;
    float travel = 0.f;
    float cameraTravel = 0.f;
    float cameraShift = 0.f;
    unsigned motionFrames = 120;
    bool rebase = false;
    float rotation = 0.f;
    float lightScale = 1.f;
    std::string sky;
    unsigned frames = 240;
    unsigned motionFrame = 120;
    std::vector<unsigned> sampleFrames;
    std::filesystem::path image, profile;
};

MeshNode* box(Scene& scene, ResourceManager& resources, const char* name,
              glm::vec3 position, glm::vec3 scale, glm::vec3 albedo,
              bool metal = false, bool optOut = false) {
    // Exercise the material reader as well as the shading opt-out.
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
    settings.giQuality = f.quality;
    settings.giMode = GIMode::FullRealtime;
    settings.giVolume = f.follow ? GIVolumeMode::Follow : GIVolumeMode::Anchored;
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
    const float corridorLength=2.f*std::abs(f.cameraTravel);
    box(scene, resources, "Floor", {0, -.4f, 0}, {18, .8f, 18+corridorLength}, {.2f, .2f, .2f},
        f.name == "metal", f.optOut);
    if (f.name == "lod" || f.name == "canopy") {
        auto* group = scene.createChild<Node>();
        group->setName("Canopy");
        auto* canopy = box(scene, resources, "Near", {0, 3, 0}, {8, .8f, 8}, glm::vec3(.2f));
        group->addChild(scene.detachChild(canopy));
        auto* far = group->createChild<Node>();
        far->setName("Far"); // An impostor contributes no geometric GI voxels.
        auto* lod = group->addBehaviour<LODGroupBehaviour>();
        lod->setLevels({{"Near", 0.f}, {"Far", 0.f}});
        lod->setCrossFade(.5f);
        // Exercise visibility ownership with real loaded mesh resources.
        // The cached walk (GIVolume's) must answer as the plain one after
        // every change, and walk nothing when nothing changed.
        GIContributorCache cache;
        auto require = [](bool ok) { if (!ok) throw std::runtime_error("GI contributor selection failed"); };
        auto contributors = [&] {
            std::vector<MeshNode*> result, cached;
            collectGIContributors(scene, result);
            cache.collect(scene, cached);
            require(cached == result);
            std::vector<MeshNode*> again;
            cache.collect(scene, again);
            require(again == result && cache.walked() == 0);
            result.clear();
            collectGIContributors(*group, result);
            return result;
        };
        lod->setEnabled(false);
        require(contributors() == std::vector<MeshNode*>{canopy});
        lod->setEnabled(true);
        require(contributors() == std::vector<MeshNode*>{canopy});
        canopy->setVisible(false);
        require(contributors() == std::vector<MeshNode*>{canopy});
        group->setVisible(false);
        require(contributors().empty());
        group->setVisible(true);
        canopy->setEnabled(false);
        require(contributors().empty());
        canopy->setEnabled(true);
        lod->setLevelReady(0, false);
        require(contributors().empty());
        lod->setLevelReady(0, true);
        require(contributors() == std::vector<MeshNode*>{canopy});
        canopy->setVisible(true);
        return;
    }
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
        box(scene, resources, "Receiver", {-2.5f, 2, 0}, {1, 4, 12+corridorLength}, glm::vec3(.2f), false, f.optOut);
        const glm::vec3 colour = f.name == "canyon-blue" ? glm::vec3(.04f, .04f, .3f)
                                                                      : glm::vec3(.3f, .04f, .04f);
        box(scene, resources, "Reflector", {2.5f, 2, 0}, {1, 4, 12+corridorLength}, colour);
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
        f.name != "canyon-red" && f.name != "canyon-blue" && f.name != "lod" && f.name != "canopy")
        throw std::invalid_argument("unknown GI fixture scene");
    f.gi = j.at("gi").get<bool>();
    const int quality=j.value("quality",0);
    if(quality<0 || quality>4)throw std::invalid_argument("quality must be 0..4");
    f.quality=GIQuality(quality);
    f.optOut = j.value("optOut", false);
    f.follow = j.value("follow", false);
    f.travel = j.value("travel", 0.f);
    f.cameraTravel = j.value("cameraTravel", 0.f);
    f.cameraShift = j.value("cameraShift", 0.f);
    f.motionFrames = j.value("motionFrames", 120u);
    f.rebase = j.value("rebase", false);
    f.rotation = j.value("rotation", 0.f);
    f.lightScale = j.value("lightScale", 1.f);
    f.sky = j.value("sky", std::string{});
    f.frames = j.value("frames", 240u);
    f.motionFrame = j.value("motionFrame", 120u);
    f.sampleFrames = j.value("sampleFrames", std::vector<unsigned>{});
    f.image = j.at("image").get<std::string>();
    f.profile = j.value("profile", std::string{});
    if (f.frames == 0 || !std::isfinite(f.travel) || !std::isfinite(f.rotation) ||
        !std::isfinite(f.cameraTravel) || !std::isfinite(f.cameraShift) || !std::isfinite(f.lightScale) || f.lightScale < 0 || f.motionFrames == 0)
        throw std::invalid_argument("invalid GI capture parameters");
    unsigned previous = f.frames;
    for (unsigned frame : f.sampleFrames) {
        if (frame <= previous) throw std::invalid_argument("sample frames must increase after the first capture");
        previous = frame;
    }
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
            ++frame;
            if(f.cameraTravel!=0.f && frame>=f.motionFrame) {
                const float fraction=std::min(float(frame-f.motionFrame+1)/float(f.motionFrames),1.f);
                const glm::vec3 shift(0,0,-f.cameraTravel*fraction);
                engine.setCameraOverride(eye+shift,target+shift);
            }
            if (frame != f.motionFrame) return;
            if (f.cameraShift != 0.f) {
                const glm::vec3 offset(f.cameraShift, 0, 0);
                engine.setCameraOverride(eye+offset, target+offset);
            }
            if (f.name == "lod") {
                auto* group = engine.scene().findByPath("Canopy");
                group->getBehaviour<LODGroupBehaviour>()->setFinestLevel(1);
            }
            for (auto* light : engine.scene().lights()) light->intensity *= f.lightScale;
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
        // Resume the same world and atlases: separate processes can rasterize
        // overlapping voxel materials differently and are not a temporal test.
        unsigned previous = f.frames;
        for (unsigned frame : f.sampleFrames) {
            if (engine.captureFailed()) break;
            glfwSetWindowShouldClose(engine.window().handle(), GLFW_FALSE);
            shot.frame = frame - previous;
            shot.pngPath = (f.image.parent_path() /
                (f.image.stem().string() + "-" + std::to_string(frame) + ".png")).generic_string();
            engine.captureFrameThenExit(shot);
            engine.run();
            previous = frame;
        }
        return engine.captureFailed() ? EXIT_FAILURE : EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "GI fixture: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
