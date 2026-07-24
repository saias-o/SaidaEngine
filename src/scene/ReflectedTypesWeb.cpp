// Web-only minimal implementation of registerReflectedTypes().
//
// The desktop/editor build uses ReflectedTypes.cpp — the full catalog of
// reflected behaviours (physics character, audio, scenario, vehicles, …) and
// nodes. The web runtime is a curated source subset (web/runtime/CMakeLists.txt)
// that deliberately excludes physics/Jolt, audio, QuickJS and scenario
// behaviours to keep the WASM small. It must NOT pull that whole graph in.
//
// SaidaOpApplier (linked into the web runtime) still calls registerReflectedTypes()
// + NodeRegistry::create() for the create_node op, so the web build needs *a*
// definition. Here we register the reflected render nodes; SceneSnapshot adds
// the base, Mesh and durable HUD factories before verifying the canonical
// authoringWasm matrix. No behaviours are registered — the
// add_behaviour / set_behaviour_property ops simply report the type as unknown on
// web, which is the correct MVP behavior (no behaviours on the web runtime yet).
//
// This whole TU is Emscripten-guarded so it never collides with the desktop
// ReflectedTypes.cpp definition.
#ifdef __EMSCRIPTEN__

#include "scene/ReflectedTypes.hpp"

#include "core/Reflection.hpp"
#include "scene/NodeRegistry.hpp"

#include "nodes/CameraNode.hpp"
#include "nodes/LightNode.hpp"
#include "scene/Node.hpp"
#include "nodes/ParticleSystemNode.hpp"
#include "nodes/WaterNode.hpp"

namespace saida {
namespace {

// Mirrors ReflectedTypes.cpp::registerNode(): copy the reflected descriptor into
// the global registry, tag it, and wire the NodeRegistry factory.
template <typename T>
void registerNode() {
    reflect::TypeDesc& d = reflect::TypeRegistry::instance().add(T::reflectName());
    d = reflect::localDesc<T>();
    d.name = T::reflectName();
    d.category = "node";
    NodeRegistry::instance().registerType<T>(T::reflectName());
}

} // namespace

void registerReflectedTypes() {
    static bool registered = false;
    if (registered) return;
    registered = true;

    NodeRegistry::instance().registerType<Node>("Node");
    NodeRegistry::instance().registerType<CameraNode>("Camera");
    registerNode<LightNode>();
    registerNode<WaterNode>();
    registerNode<ParticleSystemNode>();
}

} // namespace saida

#endif // __EMSCRIPTEN__
