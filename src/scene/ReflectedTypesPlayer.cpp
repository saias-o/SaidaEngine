// Player-web implementation of registerReflectedTypes().
//
// The web player (web/player) runs the real game loop — SceneTree,
// behaviours, signals, QuickJS scripts, Jolt physics (single-threaded job
// system), and miniaudio audio (Web Audio backend). The UICanvas/UIText HUD
// is ported; other UI types remain unsupported and are rejected at preflight.
//
// Emscripten-only: never collides with ReflectedTypes.cpp (desktop) or
// ReflectedTypesWeb.cpp (authoring viewer, a separate executable).
#ifdef __EMSCRIPTEN__

#include "scene/ReflectedTypes.hpp"

#include "core/Reflection.hpp"
#include "scene/BehaviourRegistry.hpp"
#include "scene/NodeRegistry.hpp"

#include "audio/AudioSourceBehaviour.hpp"
#include "physics/AreaNode.hpp"
#include "physics/CharacterBodyNode.hpp"
#include "physics/JointNodes.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "physics/RigidBodyNode.hpp"
#include "physics/StaticBodyNode.hpp"
#include "scene/Blackboard.hpp"
#include "behaviours/CameraFollowBehaviour.hpp"
#include "behaviours/AnimGraphBehaviour.hpp"
#include "behaviours/CharacterBehaviour.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/ParticleSystemNode.hpp"
#include "behaviours/RotatorBehaviour.hpp"
#include "behaviours/SpawnerBehaviour.hpp"
#include "behaviours/StateMachineBehaviour.hpp"
#include "nodes/WaterNode.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/SequenceDirectorBehaviour.hpp"
#include "scripting/ScriptBehaviour.hpp"

namespace saida {
namespace {

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
    registerBehaviour<SpawnerBehaviour>();
    registerBehaviour<Blackboard>();
    registerBehaviour<StateMachineBehaviour>();
    registerBehaviour<SequenceDirectorBehaviour>();
    registerBehaviour<AnimGraphBehaviour>();
    registerBehaviour<CharacterBehaviour>();
    registerBehaviour<CameraFollowBehaviour>();
    registerBehaviour<AudioSourceBehaviour>();
    // Signals-only descriptor (animationEvent) — serialization stays manual.
    registerBehaviour<Animator>();
    // Descriptor without properties — the script payload keeps its
    // hand-written save()/load().
    registerBehaviour<ScriptBehaviour>();

    registerNode<LightNode>();
    registerNode<WaterNode>();
    registerNode<ParticleSystemNode>();
    registerNode<AreaNode>();
    // Physics joints (V1: fixed, point, hinge) — required in the web player.
    registerNode<FixedJointNode>();
    registerNode<PointJointNode>();
    registerNode<HingeJointNode>();
    NodeRegistry::instance().registerType<CollisionShapeNode>("CollisionShape");
    NodeRegistry::instance().registerType<StaticBodyNode>("StaticBody");
    NodeRegistry::instance().registerType<RigidBodyNode>("RigidBody");
    NodeRegistry::instance().registerType<CharacterBodyNode>("CharacterBody");
}

} // namespace saida

#endif // __EMSCRIPTEN__
