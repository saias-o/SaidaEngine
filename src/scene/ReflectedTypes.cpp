#include "scene/ReflectedTypes.hpp"

#include "core/Reflection.hpp"
#include "audio/AudioSourceBehaviour.hpp"
#include "scene/BehaviourRegistry.hpp"
#include "scene/NodeRegistry.hpp"

// Reflected types (each declares a static describe() and the SAIDA_REFLECT_* macro).
#include "scene/Blackboard.hpp"
#include "behaviours/CameraFollowBehaviour.hpp"
#include "behaviours/CharacterBehaviour.hpp"
#include "behaviours/HealthBehaviour.hpp"
#include "behaviours/LODGroupBehaviour.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/ParticleSystemNode.hpp"
#include "nodes/WaterNode.hpp"
#include "behaviours/RotatorBehaviour.hpp"
#include "behaviours/SpawnerBehaviour.hpp"
#include "behaviours/StateMachineBehaviour.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/SequenceDirectorBehaviour.hpp"
#include "scripting/ScriptBehaviour.hpp"
#include "physics/AreaNode.hpp"
#include "physics/JointNodes.hpp"
#include "scenario/ScenarioAnchor.hpp"
#include "scenario/ScenarioDirector.hpp"
#include "scenario/ScenarioRunnerBehaviour.hpp"
// <<SAIDA_MCP_INCLUDES>>  (write_cpp_behaviour inserts generated #includes above this line)

namespace saida {
namespace {

// Copy the reflected descriptor into the global registry under its public name,
// tag the category, and wire the matching factory.
template <typename T>
void registerBehaviour() {
    reflect::TypeDesc& d = reflect::TypeRegistry::instance().add(T::reflectName());
    d = reflect::localDesc<T>();
    d.name = T::reflectName();
    d.category = "behaviour";
    BehaviourRegistry::instance().registerType<T>(T::reflectName());
}

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

    registerBehaviour<RotatorBehaviour>();
    // Signals-only descriptor (animationEvent) — serialization stays manual.
    registerBehaviour<Animator>();
    // Descriptors without properties — serialization stays the hand-written
    // save()/load() (script payload) or lives on MeshNode (LOD chain).
    registerBehaviour<ScriptBehaviour>();
    registerBehaviour<LODGroupBehaviour>();
    registerBehaviour<AudioSourceBehaviour>();
    registerBehaviour<CameraFollowBehaviour>();
    registerBehaviour<CharacterBehaviour>();
    registerBehaviour<HealthBehaviour>();
    registerBehaviour<SpawnerBehaviour>();
    registerBehaviour<Blackboard>();
    registerBehaviour<StateMachineBehaviour>();
    registerBehaviour<SequenceDirectorBehaviour>();
    registerBehaviour<ScenarioAnchor>();
    registerBehaviour<ScenarioDirector>();
    registerBehaviour<ScenarioRunnerBehaviour>();
    registerNode<LightNode>();
    registerNode<WaterNode>();
    registerNode<ParticleSystemNode>();
    registerNode<AreaNode>();
    // Physics joints (V1: fixed, point, hinge) — matrix {R, R, A, R}.
    registerNode<FixedJointNode>();
    registerNode<PointJointNode>();
    registerNode<HingeJointNode>();
    // <<SAIDA_MCP_REGISTER>>  (write_cpp_behaviour inserts registerBehaviour<T>() calls above this line)
}

} // namespace saida
