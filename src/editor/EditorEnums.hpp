#pragma once

namespace saida {

enum class CreateNodeType {
    None = 0,
    Node,
    MeshNode,
    DirectionalLight,
    PointLight,
    SpotLight,
    Camera,
    SceneInstance,
    UICanvas,
    UIColorNode,
    UIImageNode,
    UITextNode,
    UIButtonNode,
    UIToggleNode,
    UIExample,
    WebCanvas,
    StaticBody,
    RigidBody,
    CharacterBody,
    Area,
    CollisionShape,
    FixedJoint,
    PointJoint,
    HingeJoint,
    Water,
    ParticleSystem
};

enum class GizmoMode {
    Translate = 0,
    Rotate,
    Scale
};

enum class BuildConfig {
    Debug = 0,
    Release,
    Profile
};

enum class BuildPlatform {
    Windows = 0,
    MetaQuest,
    Linux,
    WebGL
};

enum class GizmoAxis : int {
    None = -1,
    X = 0,
    Y = 1,
    Z = 2
};

} // namespace saida
