#include "editor/Command.hpp"
#include "editor/CommandHistory.hpp"
#include "editor/SceneDocument.hpp"
#include "graphics/Mesh.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "scene/Node.hpp"
#include "scene/Scene.hpp"
#include "nodes/WebCanvasNode.hpp"
#include "ui/RmlUiRuntime.hpp"

#include <cassert>
#include <memory>
#include <string>
#include <vector>

namespace {

class CountingCommand final : public saida::Command {
public:
    explicit CountingCommand(int& value) : value_(value) {}
    void execute(saida::SceneDocument&) override { ++value_; }
    void undo(saida::SceneDocument&) override { --value_; }

private:
    int& value_;
};

// History budget: pushing past the cap keeps the most recent commands and the
// undo/redo stacks stay consistent.
void testHistoryBudget() {
    saida::SceneDocument document;
    saida::CommandHistory history(document);
    int value = 0;

    for (int i = 0; i < 300; ++i)
        history.execute(std::make_unique<CountingCommand>(value));

    assert(value == 300);
    assert(history.undoCount() == 256);
    history.undo();
    assert(value == 299);
    assert(history.redoCount() == 1);
    history.redo();
    assert(value == 300);
}

// SetPropertyCommand resolves its node by id every time and is a clean undo/redo
// round-trip, even though it carries only type-erased apply closures.
void testSetPropertyCommand() {
    saida::Scene scene;
    saida::SceneDocument document;
    document.bind(&scene, nullptr);

    saida::Node* node = scene.createChild<saida::Node>("Original");
    const saida::NodeId id = node->id();

    saida::CommandHistory history(document);
    history.execute(std::make_unique<saida::SetPropertyCommand>(
        id, "Rename",
        [](saida::Node& n) { n.setName("Original"); },
        [](saida::Node& n) { n.setName("Edited"); }));

    assert(document.find(id)->name() == "Edited");
    assert(document.dirty());

    history.undo();
    assert(document.find(id)->name() == "Original");

    history.redo();
    assert(document.find(id)->name() == "Edited");
}

// The WebCanvas inspector edits are the same full-snapshot commands built here:
// startup-script list mutations and the URL swap round-trip through undo/redo
// on an uninitialized (GPU-free) node.
void testWebCanvasCommands() {
    saida::Scene scene;
    saida::SceneDocument document;
    document.bind(&scene, nullptr);

    auto* canvas = static_cast<saida::WebCanvasNode*>(
        scene.addChild(std::make_unique<saida::WebCanvasNode>()));
    canvas->addStartupScript("hud.init()");
    const saida::NodeId id = canvas->id();

    saida::CommandHistory history(document);

    const auto setScripts = [](saida::Node& n, std::vector<std::string> v) {
        static_cast<saida::WebCanvasNode&>(n).setStartupScripts(std::move(v));
    };

    // Add
    std::vector<std::string> before = canvas->startupScripts();
    std::vector<std::string> withNew = before;
    withNew.emplace_back("hud.show()");
    history.execute(std::make_unique<saida::SetPropertyCommand>(
        id, "Add WebCanvas Script",
        [setScripts, before](saida::Node& n) { setScripts(n, before); },
        [setScripts, withNew](saida::Node& n) { setScripts(n, withNew); }));
    assert(canvas->startupScripts().size() == 2);

    // Edit element 1 (full-list snapshot, as the inspector commits it)
    std::vector<std::string> edited = canvas->startupScripts();
    const std::vector<std::string> editOld = edited;
    edited[1] = "hud.show(true)";
    history.execute(std::make_unique<saida::SetPropertyCommand>(
        id, "Edit WebCanvas Script",
        [setScripts, editOld](saida::Node& n) { setScripts(n, editOld); },
        [setScripts, edited](saida::Node& n) { setScripts(n, edited); }));
    assert(canvas->startupScripts()[1] == "hud.show(true)");

    // Remove element 0
    std::vector<std::string> removeOld = canvas->startupScripts();
    std::vector<std::string> without = removeOld;
    without.erase(without.begin());
    history.execute(std::make_unique<saida::SetPropertyCommand>(
        id, "Remove WebCanvas Script",
        [setScripts, removeOld](saida::Node& n) { setScripts(n, removeOld); },
        [setScripts, without](saida::Node& n) { setScripts(n, without); }));
    assert(canvas->startupScripts().size() == 1);
    assert(canvas->startupScripts()[0] == "hud.show(true)");

    // LIFO undo restores each exact list state.
    history.undo();
    assert(canvas->startupScripts().size() == 2 &&
           canvas->startupScripts()[1] == "hud.show(true)");
    history.undo();
    assert(canvas->startupScripts()[1] == "hud.show()");
    history.undo();
    assert(canvas->startupScripts().size() == 1 &&
           canvas->startupScripts()[0] == "hud.init()");
    history.redo();
    history.redo();
    history.redo();
    assert(canvas->startupScripts().size() == 1 &&
           canvas->startupScripts()[0] == "hud.show(true)");

    // URL edit (side-effectful setter: reloads document state; safe headless).
    const std::string oldUrl = canvas->url();
    history.execute(std::make_unique<saida::SetPropertyCommand>(
        id, "Set WebCanvas URL",
        [oldUrl](saida::Node& n) { static_cast<saida::WebCanvasNode&>(n).setUrl(oldUrl); },
        [](saida::Node& n) { static_cast<saida::WebCanvasNode&>(n).setUrl("ui/menu.rml"); }));
    assert(canvas->url() == "ui/menu.rml");
    history.undo();
    assert(canvas->url() == oldUrl);
}

// The collision-shape commands snapshot the full durable state. The subtle
// contract: undoing "Recompute" restores the captured parameters WITHOUT
// re-arming Auto, so the still-valid detection cache keeps them frozen.
void testCollisionShapeCommands() {
    saida::Scene scene;
    saida::SceneDocument document;
    document.bind(&scene, nullptr);

    auto* shape = static_cast<saida::CollisionShapeNode*>(
        scene.addChild(std::make_unique<saida::CollisionShapeNode>()));
    const saida::NodeId id = shape->id();

    // Freeze an initial Auto detection. Extents 2x3x6 hit the Box branch of the
    // heuristic (mid/short >= 1.4 rules out the capsule), so halfExtents is the
    // parameter the detection rewrites.
    saida::Aabb bounds{glm::vec3(-1.0f, -1.5f, -3.0f), glm::vec3(1.0f, 1.5f, 3.0f)};
    assert(shape->resolveAutoFrom(bounds, glm::mat4(1.0f)));
    const glm::vec3 frozenExtents = shape->halfExtents;

    saida::CommandHistory history(document);

    struct ShapeState {
        saida::CollisionShapeType type;
        glm::vec3 halfExtents;
        float radius;
        float height;
        int axis;
        glm::vec3 offset;
    };
    const auto capture = [](const saida::CollisionShapeNode& s) {
        return ShapeState{s.shapeType, s.halfExtents, s.radius, s.height, s.axis, s.offset};
    };
    const auto apply = [](saida::Node& n, const ShapeState& s, bool rearmAuto) {
        auto& node = static_cast<saida::CollisionShapeNode&>(n);
        node.shapeType = s.type;
        node.halfExtents = s.halfExtents;
        node.radius = s.radius;
        node.height = s.height;
        node.axis = s.axis;
        node.offset = s.offset;
        if (rearmAuto && s.type == saida::CollisionShapeType::Auto) node.resetAuto();
    };

    // Type switch Auto -> Sphere, then undo back to Auto.
    {
        ShapeState before = capture(*shape);
        ShapeState after = before;
        after.type = saida::CollisionShapeType::Sphere;
        history.execute(std::make_unique<saida::SetPropertyCommand>(
            id, "Set Collision Shape Type",
            [apply, before](saida::Node& n) { apply(n, before, false); },
            [apply, after](saida::Node& n) { apply(n, after, true); }));
        assert(shape->shapeType == saida::CollisionShapeType::Sphere);
        history.undo();
        assert(shape->shapeType == saida::CollisionShapeType::Auto);
        assert(shape->halfExtents == frozenExtents);
        // Undo did not re-arm Auto: the detection cache still holds.
        assert(!shape->resolveAutoFrom(bounds, glm::mat4(1.0f)));
    }

    // Recompute against a grown mesh, then undo back to the frozen values.
    {
        ShapeState before = capture(*shape);
        history.execute(std::make_unique<saida::SetPropertyCommand>(
            id, "Recompute Collision Shape",
            [apply, before](saida::Node& n) { apply(n, before, false); },
            [apply, before](saida::Node& n) { apply(n, before, true); }));

        // The next resolve (the live frame after the click) re-detects from the
        // current, larger mesh and rewrites the parameters (still a Box: 4x6x12).
        saida::Aabb grown{glm::vec3(-2.0f, -3.0f, -6.0f), glm::vec3(2.0f, 3.0f, 6.0f)};
        assert(shape->resolveAutoFrom(grown, glm::mat4(1.0f)));
        assert(shape->halfExtents != frozenExtents);

        history.undo();
        assert(shape->halfExtents == frozenExtents);
        // Frozen again: another resolve against the same mesh must NOT rewrite
        // the restored parameters (this is the no-rearm-on-undo contract).
        assert(!shape->resolveAutoFrom(grown, glm::mat4(1.0f)));
        assert(shape->halfExtents == frozenExtents);
    }
}

} // namespace

int main() {
    testHistoryBudget();
    testSetPropertyCommand();
    testWebCanvasCommands();
    testCollisionShapeCommands();
    // The URL test may have booted RmlUi; every Rml context died with its scene,
    // so shut the library down before its static interfaces unwind at exit.
    saida::RmlUiRuntime::shutdown();
    return 0;
}
