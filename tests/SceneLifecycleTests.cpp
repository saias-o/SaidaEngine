#include "scene/Behaviour.hpp"
#include "scene/Node.hpp"

#include <cassert>

namespace {

class ProbeBehaviour final : public saida::Behaviour {
public:
    ProbeBehaviour(bool& ready, bool& destroyed)
        : ready_(ready), destroyed_(destroyed) {}

    void onReady() override { ready_ = true; }
    void onDestroy() override { destroyed_ = true; }

private:
    bool& ready_;
    bool& destroyed_;
};

} // namespace

int main() {
    saida::Node node("LifecycleProbe");
    saida::Node another("IdentityProbe");
    assert(node.id() != saida::kNodeInvalid);
    assert(another.id() != saida::kNodeInvalid);
    assert(node.id() != another.id());

    const saida::NodeId preserved = node.id();
    node.assignSerializedId(preserved);
    assert(node.id() == preserved);
    node.regenerateId();
    assert(node.id() != preserved);

    bool ready = false;
    bool destroyed = false;
    auto* behaviour = node.addBehaviour<ProbeBehaviour>(ready, destroyed);

    node.updateTree(0.0f);
    assert(ready);
    assert(!destroyed);

    node.removeBehaviour(behaviour);
    assert(destroyed);
    assert(node.behaviourCount() == 0);
    return 0;
}
