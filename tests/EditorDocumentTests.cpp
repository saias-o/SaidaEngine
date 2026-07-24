// Lot 8 — validation finale des invariants de l'editeur refactorise.
//
// Couverture :
//   - SceneDocument : bind / find / selection / dirty lifecycle
//   - RenameNodeCommand       : round-trip execute / undo / redo
//   - TransformCommand        : round-trip execute / undo / redo
//   - ReparentNodeCommand     : deplacement de noeud + undo
//   - CommandHistory extras   : clear(), redo-cleared-on-new, undoName/redoName

#include "editor/Command.hpp"
#include "editor/CommandHistory.hpp"
#include "editor/SceneDocument.hpp"
#include "scene/Node.hpp"
#include "scene/Scene.hpp"

#include <cassert>
#include <cmath>
#include <memory>
#include <string>

namespace {

// SceneDocument: bind/find/dirty/selection lifecycle.
void testSceneDocumentState() {
    saida::Scene scene;
    saida::SceneDocument document;

    // Avant bind : find renvoie toujours nullptr
    assert(document.find(saida::kNodeInvalid) == nullptr);
    assert(!document.dirty());

    document.bind(&scene, nullptr);
    assert(document.find(saida::kNodeInvalid) == nullptr);

    saida::Node* node = scene.createChild<saida::Node>("A");
    const saida::NodeId id = node->id();
    assert(document.find(id) == node);

    // dirty / saved / loaded
    document.markDirty();
    assert(document.dirty());
    document.markSaved();
    assert(!document.dirty());
    document.markDirty();
    document.markLoaded();  // vide dirty + selection
    assert(!document.dirty());

    // select / clearSelection
    document.select(node);
    assert(document.selectedId() == id);
    assert(document.selectedNode() == node);
    document.clearSelection();
    assert(document.selectedId() == saida::kNodeInvalid);
    assert(document.selectedNode() == nullptr);

    // Selection perimee : supprimer le noeud puis re-binder -> selection effacee
    document.select(node);
    assert(document.selectedId() == id);
    scene.removeChild(node);         // node est detruit ici
    document.bind(&scene, nullptr);  // find(id) == nullptr -> clearSelection
    assert(document.selectedId() == saida::kNodeInvalid);
}

// RenameNodeCommand : execute renomme, undo restaure, redo re-renomme.
// Chaque operation marque le document dirty.
void testRenameCommandRoundTrip() {
    saida::Scene scene;
    saida::SceneDocument document;
    document.bind(&scene, nullptr);

    saida::Node* node = scene.createChild<saida::Node>("OldName");
    const saida::NodeId id = node->id();

    saida::CommandHistory history(document);
    history.execute(std::make_unique<saida::RenameNodeCommand>(id, "OldName", "NewName"));
    assert(document.find(id)->name() == "NewName");
    assert(document.dirty());

    document.markSaved();
    history.undo();
    assert(document.find(id)->name() == "OldName");
    assert(document.dirty());  // undo marque aussi dirty

    history.redo();
    assert(document.find(id)->name() == "NewName");
}

// TransformCommand : execute applique la nouvelle transform, undo restitue l'ancienne.
void testTransformCommandRoundTrip() {
    saida::Scene scene;
    saida::SceneDocument document;
    document.bind(&scene, nullptr);

    saida::Node* node = scene.createChild<saida::Node>("T");
    const saida::NodeId id = node->id();

    saida::Transform oldT;  // position (0,0,0) par defaut
    saida::Transform newT;
    newT.position = {1.f, 2.f, 3.f};

    saida::CommandHistory history(document);
    history.execute(std::make_unique<saida::TransformCommand>(id, oldT, newT));

    {
        const auto& p = document.find(id)->transform().position;
        assert(std::abs(p.x - 1.f) < 1e-5f);
        assert(std::abs(p.y - 2.f) < 1e-5f);
        assert(std::abs(p.z - 3.f) < 1e-5f);
    }
    assert(document.dirty());

    history.undo();
    {
        const auto& p = document.find(id)->transform().position;
        assert(std::abs(p.x) < 1e-5f);
        assert(std::abs(p.y) < 1e-5f);
        assert(std::abs(p.z) < 1e-5f);
    }

    history.redo();
    {
        const auto& p = document.find(id)->transform().position;
        assert(std::abs(p.x - 1.f) < 1e-5f);
    }
}

// ReparentNodeCommand : le noeud change de parent; undo restitue le parent d'origine.
void testReparentCommandRoundTrip() {
    saida::Scene scene;
    saida::SceneDocument document;
    document.bind(&scene, nullptr);

    saida::Node* parentA = scene.createChild<saida::Node>("ParentA");
    saida::Node* child   = parentA->createChild<saida::Node>("Child");
    saida::Node* parentB = scene.createChild<saida::Node>("ParentB");
    const saida::NodeId childId   = child->id();
    const saida::NodeId parentBId = parentB->id();

    assert(parentA->children().size() == 1);
    assert(parentB->children().size() == 0);

    saida::CommandHistory history(document);
    history.execute(std::make_unique<saida::ReparentNodeCommand>(childId, parentBId));

    assert(parentA->children().size() == 0);
    assert(parentB->children().size() == 1);
    assert(document.find(childId) != nullptr);
    assert(document.dirty());

    history.undo();
    assert(parentA->children().size() == 1);
    assert(parentB->children().size() == 0);
    assert(document.find(childId)->name() == "Child");
}

// CommandHistory extras :
//   - Executer une commande apres un undo vide le redo stack (historique lineaire).
//   - clear() vide les deux piles ; undoName/redoName renvoient "" quand vides.
void testHistoryClearAndRedoOnNew() {
    saida::Scene scene;
    saida::SceneDocument document;
    document.bind(&scene, nullptr);

    saida::Node* node = scene.createChild<saida::Node>("N");
    const saida::NodeId id = node->id();
    saida::CommandHistory history(document);

    history.execute(std::make_unique<saida::RenameNodeCommand>(id, "N", "A"));
    history.execute(std::make_unique<saida::RenameNodeCommand>(id, "A", "B"));
    history.execute(std::make_unique<saida::RenameNodeCommand>(id, "B", "C"));
    assert(history.undoCount() == 3);
    assert(history.redoCount() == 0);

    history.undo();
    assert(history.undoCount() == 2);
    assert(history.redoCount() == 1);
    assert(std::string(history.redoName()) == "Rename Node");

    // Nouvelle commande : redo stack efface
    history.execute(std::make_unique<saida::RenameNodeCommand>(id, "B", "D"));
    assert(history.redoCount() == 0);
    assert(history.undoCount() == 3);
    assert(std::string(history.undoName()) == "Rename Node");

    // clear() remet tout a zero
    history.clear();
    assert(history.undoCount() == 0);
    assert(history.redoCount() == 0);
    assert(!history.canUndo());
    assert(!history.canRedo());
    assert(std::string(history.undoName()) == "");
    assert(std::string(history.redoName()) == "");
}

} // namespace

int main() {
    testSceneDocumentState();
    testRenameCommandRoundTrip();
    testTransformCommandRoundTrip();
    testReparentCommandRoundTrip();
    testHistoryClearAndRedoOnNew();
    return 0;
}
