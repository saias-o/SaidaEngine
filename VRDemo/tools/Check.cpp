#include "project/Project.hpp"
#include "project/ProjectRename.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/Scene.hpp"
#include "scene/NodeRegistry.hpp"
#include "scene/ReflectedTypes.hpp"
#include "nodes/CameraNode.hpp"
#include "nodes/MeshNode.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "physics/StaticBodyNode.hpp"
#include "physics/RigidBodyNode.hpp"
#include "xr/XrRegistration.hpp"
#include "xr/toolkit/XRController.hpp"
#include "xr/toolkit/XRGrabbable.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace saida;
using json = nlohmann::json;
namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// Only the native registries and physics are needed: no Engine, Vulkan,
// OpenXR session, window, audio device or renderer is started by this tool.
void registerSceneTypes() {
    registerReflectedTypes();
    xr::registerTypes();
    auto& registry = NodeRegistry::instance();
    registry.registerType<Node>("Node");
    registry.registerType<Scene>("Scene");
    registry.registerType<MeshNode>("MeshNode");
    registry.registerType<CameraNode>("Camera");
    registry.registerType<CollisionShapeNode>("CollisionShape");
    registry.registerType<StaticBodyNode>("StaticBody");
    registry.registerType<RigidBodyNode>("RigidBody");
}

int main(int argc, char** argv) {
    try {
        const fs::path root = fs::absolute(VRDEMO_ROOT).lexically_normal();
        Project project;
        if (argc == 3 && std::string(argv[1]) == "--rename-project") {
            const auto renamed = renameProjectDirectory(root.string(), argv[2], "");
            require(renamed.ok, renamed.error);
            std::cout << renamed.projectFilePath << '\n';
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--create-project") {
            require(!fs::exists(root / "VRDemo.saidaproj"), "Project already exists; refusing to overwrite it");
            require(project.create(root.parent_path().string(), "VRDemo"), "Project::create failed");
            project.setMainScene("scenes/main.scene");
            project.setMaxFps(90);
            project.setVSync(false);
            project.setShowColliders(false);
            require(project.save(), "Project::save failed");
            return 0;
        }
        require(project.load((root / "VRDemo.saidaproj").string()), "Project::load failed");
        const fs::path scenePath = root / project.mainScene();
        require(SceneSerializer::validateSceneDocumentFile(scenePath.string()), "Invalid runtime scene envelope");
        std::ifstream input(scenePath);
        const json document = json::parse(input);
        registerSceneTypes();
        std::string error;
        require(SceneSerializer::validateTypeContractJson(document.at("scene").dump(), &error), error);

        const json* cubeDocument = nullptr;
        size_t origins = 0, hands = 0, interactors = 0;
        for (const auto& node : document.at("scene").at("children")) {
            const std::string type = node.at("type");
            if (type == "XROrigin") ++origins;
            if (type == "XRHand") ++hands;
            if (type == "XRController") {
                for (const auto& behaviour : node.at("behaviours"))
                    if (behaviour.at("type") == "XRDirectInteractor") ++interactors;
            }
            if (node.value("name", "") == "Grab Cube") cubeDocument = &node;
        }
        require(origins == 1 && hands == 2 && interactors == 2, "Scene needs one origin and two tracked hands/interactors");
        require(cubeDocument && cubeDocument->at("type") == "RigidBody", "Grab cube must be a RigidBody");

        // Exercise the production grabbable on the scene's physical cube with
        // each hand: hold, move, release, then let the real solver catch it.
        for (const auto side : {XRHand::Left, XRHand::Right}) {
            Scene simulation;
            auto* floor = simulation.createChild<StaticBodyNode>();
            floor->transform().position = {0, -0.05f, 0};
            auto* floorShape = floor->createChild<CollisionShapeNode>();
            floorShape->shapeType = CollisionShapeType::Box;
            floorShape->halfExtents = {2, 0.05f, 2};
            auto* controller = simulation.createChild<XRController>(side);
            auto* cube = simulation.createChild<RigidBodyNode>();
            const auto& p = cubeDocument->at("transform").at("position");
            cube->transform().position = {p[0], p[1], p[2]};
            cube->mass = cubeDocument->at("mass");
            auto* shape = cube->createChild<CollisionShapeNode>();
            shape->shapeType = CollisionShapeType::Box;
            const auto& e = cubeDocument->at("children")[1].at("halfExtents");
            shape->halfExtents = {e[0], e[1], e[2]};
            auto* grab = cube->addBehaviour<XRGrabbable>();
            grab->load(cubeDocument->at("behaviours")[0]);
            simulation.update(1.0f / 90);
            grab->grab(controller);
            require(grab->isGrabbed() && cube->kinematic, "Grabbing must make the body kinematic");
            const glm::vec3 initial = cube->transform().position;
            controller->transform().position += glm::vec3(0.12f, 0.18f, 0);
            simulation.updateTransforms(glm::mat4(1), true);
            simulation.update(1.0f / 90);
            require(glm::distance(cube->transform().position, initial + glm::vec3(0.12f, 0.18f, 0)) < 0.005f,
                    "Held cube must follow the hand");
            for (int i = 0; i < 20; ++i) simulation.update(1.0f / 90);
            grab->release();
            require(!grab->isGrabbed() && !cube->kinematic, "Release must restore a dynamic body");
            for (int i = 0; i < 270; ++i) simulation.update(1.0f / 90);
            require(cube->transform().position.y > 0.04f && cube->transform().position.y < 0.09f,
                    "Released cube must fall and settle on the floor");
        }
        std::cout << "VRDemo: project, runtime scene contract and two-hand grab/release physics passed (no window).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "VRDemo: " << error.what() << '\n';
        return 1;
    }
}
