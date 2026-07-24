#include "authoring/EngineManifest.hpp"
#include "authoring/SceneSnapshot.hpp"
#include "physics/CharacterBodyNode.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "physics/RigidBodyNode.hpp"
#include "physics/StaticBodyNode.hpp"
#include "runtime/RuntimeRoundTripContract.hpp"
#include "scene/Node.hpp"
#include "scene/NodeRegistry.hpp"
#include "scene/RuntimeTypeMatrix.hpp"
#include "scene/Scene.hpp"
#include "nodes/UICanvasNode.hpp"
#include "nodes/UITextNode.hpp"

#include <cassert>
#include <memory>
#include <set>
#include <string>

int main() {
    using namespace saida;

    std::set<std::string> identities;
    RuntimeTypeCategory previousCategory = RuntimeTypeCategory::Node;
    std::string previousName;
    for (const RuntimeTypeRow& row : kRuntimeTypeMatrix) {
        const std::string identity =
            std::string(row.category == RuntimeTypeCategory::Node ? "node:" : "behaviour:") +
            row.name;
        assert(identities.insert(identity).second);
        if (row.category == previousCategory) {
            assert(previousName.empty() || previousName < row.name);
        } else {
            assert(previousCategory == RuntimeTypeCategory::Node);
            assert(row.category == RuntimeTypeCategory::Behaviour);
            previousName.clear();
        }
        previousCategory = row.category;
        previousName = row.name;
    }

    const nlohmann::json matrix = buildRuntimeTypeMatrixManifest();
    assert(matrix["schema"] == 1);
    assert(matrix["targets"].size() == 4);
    assert(matrix["types"].size() == kRuntimeTypeMatrix.size());
    assert(authoring::buildEngineManifest()["runtimeTypeMatrix"] == matrix);

    // A fresh headless process must register exactly the factories declared by
    // the canonical table. Serializing an empty scene initializes that catalog.
    Scene scene;
    auto canvas = std::make_unique<UICanvasNode>();
    canvas->setName("HUD");
    canvas->setSize(1280.0f, 720.0f);
    auto text = std::make_unique<UITextNode>();
    text->setName("Status");
    text->setPosition(24.0f, 36.0f);
    text->setSize(320.0f, 48.0f);
    text->setAnchor(0.0f, 0.0f);
    text->setPivot(0.0f, 0.0f);
    text->setText("Matrix ready");
    text->setFontSize(22.0f);
    text->setColor({0.25f, 0.5f, 0.75f, 1.0f});
    canvas->addChild(std::move(text));
    scene.addChild(std::move(canvas));

    auto rigid = std::make_unique<RigidBodyNode>();
    rigid->setName("Crate");
    rigid->friction = 0.7f;
    rigid->restitution = 0.2f;
    rigid->kinematic = true;
    rigid->mass = 12.0f;
    rigid->gravityFactor = 0.8f;
    rigid->linearDamping = 0.15f;
    rigid->angularDamping = 0.25f;
    auto shape = std::make_unique<CollisionShapeNode>();
    shape->shapeType = CollisionShapeType::Capsule;
    shape->halfExtents = {1.0f, 2.0f, 3.0f};
    shape->radius = 0.75f;
    shape->height = 3.5f;
    shape->axis = 2;
    shape->offset = {0.1f, 0.2f, 0.3f};
    rigid->addChild(std::move(shape));
    scene.addChild(std::move(rigid));

    auto character = std::make_unique<CharacterBodyNode>();
    character->setName("HeroBody");
    character->friction = 0.4f;
    character->restitution = 0.05f;
    character->mass = 82.0f;
    character->maxSlopeAngle = 47.0f;
    scene.addChild(std::move(character));

    auto staticBody = std::make_unique<StaticBodyNode>();
    staticBody->setName("FloorBody");
    staticBody->friction = 0.9f;
    staticBody->restitution = 0.1f;
    scene.addChild(std::move(staticBody));

    const std::string snapshot = authoring::serializeSceneSnapshot(scene, nullptr);
    std::string error;
    assert(verifyRegisteredRuntimeTypes(RuntimeTypeTarget::Headless, error));

    Scene reloaded;
    assert(authoring::deserializeSceneSnapshot(snapshot, reloaded, &error));
    assert(reloaded.children().size() == 4);
    auto* reloadedCanvas = dynamic_cast<UICanvasNode*>(reloaded.children()[0].get());
    assert(reloadedCanvas);
    assert(reloadedCanvas->width() == 1280.0f && reloadedCanvas->height() == 720.0f);
    assert(reloadedCanvas->children().size() == 1);
    auto* reloadedText = dynamic_cast<UITextNode*>(reloadedCanvas->children()[0].get());
    assert(reloadedText);
    assert(reloadedText->text() == "Matrix ready");
    assert(reloadedText->fontSize() == 22.0f);
    assert(reloadedText->x() == 24.0f && reloadedText->y() == 36.0f);

    auto* reloadedRigid = dynamic_cast<RigidBodyNode*>(reloaded.children()[1].get());
    assert(reloadedRigid && reloadedRigid->kinematic && reloadedRigid->mass == 12.0f);
    assert(reloadedRigid->gravityFactor == 0.8f && reloadedRigid->friction == 0.7f);
    assert(reloadedRigid->children().size() == 1);
    auto* reloadedShape =
        dynamic_cast<CollisionShapeNode*>(reloadedRigid->children()[0].get());
    assert(reloadedShape && reloadedShape->shapeType == CollisionShapeType::Capsule);
    assert(reloadedShape->radius == 0.75f && reloadedShape->axis == 2);
    auto* reloadedCharacter =
        dynamic_cast<CharacterBodyNode*>(reloaded.children()[2].get());
    assert(reloadedCharacter && reloadedCharacter->mass == 82.0f);
    assert(reloadedCharacter->maxSlopeAngle == 47.0f);
    auto* reloadedStatic = dynamic_cast<StaticBodyNode*>(reloaded.children()[3].get());
    assert(reloadedStatic && reloadedStatic->friction == 0.9f);
    assert(authoring::serializeSceneSnapshot(reloaded, nullptr) == snapshot);

    // The exhaustive resource-free headless round-trip is the shared contract
    // that the shipped saida_tool and the web authoring runtime also run: every
    // headless-promised type reconstructs identically through the snapshot codec.
    runtime::RoundTripContractReport contractReport;
    std::string contractError;
    assert(runtime::verifySnapshotRoundTripContract(
        RuntimeTypeTarget::Headless, contractReport, contractError));
    assert(contractReport.nodes > 0 && contractReport.behaviours > 0);
    assert(contractReport.reflectedProperties > 0);

    // Unknown additions are also contract drift, not only missing factories.
    NodeRegistry::instance().registerType<Node>("UnexpectedTestNode");
    error.clear();
    assert(!verifyRegisteredRuntimeTypes(RuntimeTypeTarget::Headless, error));
    assert(error.find("UnexpectedTestNode") != std::string::npos);
    return 0;
}
