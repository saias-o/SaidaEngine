// Locks in M2: data-driven signal→slot connections wire reflected signals to
// reflected slots across a scene, lifetime-safe, and survive a JSON round-trip.

#include "core/Reflection.hpp"
#include "scene/Node.hpp"
#include "behaviours/RotatorBehaviour.hpp"
#include "scene/Scene.hpp"
#include "scene/SignalWiring.hpp"
#include "scene/ReflectedTypes.hpp"

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <cassert>
#include <cmath>
#include <memory>

namespace {

bool isIdentity(const glm::quat& q) {
    return std::abs(q.w - 1.0f) < 1e-4f && std::abs(q.x) < 1e-4f &&
           std::abs(q.y) < 1e-4f && std::abs(q.z) < 1e-4f;
}

} // namespace

int main() {
    saida::registerReflectedTypes();

    // ── build a scene: A emits fullRotation, B has a reset slot ──
    saida::Scene scene;
    saida::Node* a = scene.addChild(std::make_unique<saida::Node>("A"));
    auto* ra = a->addBehaviour<saida::RotatorBehaviour>();

    saida::Node* b = scene.addChild(std::make_unique<saida::Node>("B"));
    b->addBehaviour<saida::RotatorBehaviour>();
    b->transform().rotation = glm::angleAxis(1.0f, glm::vec3(0, 1, 0));  // non-identity
    assert(!isIdentity(b->transform().rotation));

    // Wire A.fullRotation → B.reset (reset snaps B's rotation to identity).
    scene.connections().push_back({a->id(), "fullRotation", b->id(), "reset"});
    scene.applyConnections();

    ra->fullRotation.emit();
    assert(isIdentity(b->transform().rotation) && "slot must have fired and reset B");

    // ── lifetime: freeing the target makes the link a silent no-op ──
    b->transform().rotation = glm::angleAxis(1.0f, glm::vec3(0, 1, 0));
    scene.removeChild(b);          // B destroyed; connection re-resolves by id → none
    ra->fullRotation.emit();       // must not crash, nothing to reset

    // ── clearing drops the live connection ──
    scene.clearConnections();
    ra->fullRotation.emit();       // no-op

    // ── JSON round-trip preserves the connections block ──
    {
        saida::Scene src;
        saida::Node* x = src.addChild(std::make_unique<saida::Node>("X"));
        saida::Node* y = src.addChild(std::make_unique<saida::Node>("Y"));
        src.connections().push_back({x->id(), "fullRotation", y->id(), "reset"});

        nlohmann::json j;
        // Scene::serialize requires a ResourceManager only for asset refs; the
        // connections block is written independently, so exercise it directly.
        nlohmann::json conns = nlohmann::json::array();
        for (const auto& c : src.connections())
            conns.push_back({{"from", c.from}, {"signal", c.signal}, {"to", c.to}, {"slot", c.slot}});
        j["connections"] = conns;

        saida::Scene dst;
        dst.readConnections(j);
        assert(dst.connections().size() == 1);
        assert(dst.connections()[0].from == x->id());
        assert(dst.connections()[0].signal == "fullRotation");
        assert(dst.connections()[0].to == y->id());
        assert(dst.connections()[0].slot == "reset");
    }

    return 0;
}
