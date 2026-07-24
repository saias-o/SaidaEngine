#include "xr/toolkit/XRHand.hpp"

#include "graphics/Material.hpp"
#include "graphics/ResourceManager.hpp"
#include "nodes/MeshNode.hpp"
#include "xr/toolkit/XRInput.hpp"

#include <glm/gtc/constants.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace saida {

namespace {

class XRHandVisualTracker final : public Behaviour {
public:
    bool visibleInEditor() const override { return false; }
    void onUpdate(float) override {
        static_cast<XRHandNode*>(node())->updateTrackingVisuals();
    }
};

struct BonePair { XRHandJoint from; XRHandJoint to; };

constexpr BonePair kBones[] = {
    {XRHandJoint::Wrist, XRHandJoint::Palm},
    {XRHandJoint::Wrist, XRHandJoint::ThumbMetacarpal},
    {XRHandJoint::ThumbMetacarpal, XRHandJoint::ThumbProximal},
    {XRHandJoint::ThumbProximal, XRHandJoint::ThumbDistal},
    {XRHandJoint::ThumbDistal, XRHandJoint::ThumbTip},
    {XRHandJoint::Wrist, XRHandJoint::IndexMetacarpal},
    {XRHandJoint::IndexMetacarpal, XRHandJoint::IndexProximal},
    {XRHandJoint::IndexProximal, XRHandJoint::IndexIntermediate},
    {XRHandJoint::IndexIntermediate, XRHandJoint::IndexDistal},
    {XRHandJoint::IndexDistal, XRHandJoint::IndexTip},
    {XRHandJoint::Wrist, XRHandJoint::MiddleMetacarpal},
    {XRHandJoint::MiddleMetacarpal, XRHandJoint::MiddleProximal},
    {XRHandJoint::MiddleProximal, XRHandJoint::MiddleIntermediate},
    {XRHandJoint::MiddleIntermediate, XRHandJoint::MiddleDistal},
    {XRHandJoint::MiddleDistal, XRHandJoint::MiddleTip},
    {XRHandJoint::Wrist, XRHandJoint::RingMetacarpal},
    {XRHandJoint::RingMetacarpal, XRHandJoint::RingProximal},
    {XRHandJoint::RingProximal, XRHandJoint::RingIntermediate},
    {XRHandJoint::RingIntermediate, XRHandJoint::RingDistal},
    {XRHandJoint::RingDistal, XRHandJoint::RingTip},
    {XRHandJoint::Wrist, XRHandJoint::LittleMetacarpal},
    {XRHandJoint::LittleMetacarpal, XRHandJoint::LittleProximal},
    {XRHandJoint::LittleProximal, XRHandJoint::LittleIntermediate},
    {XRHandJoint::LittleIntermediate, XRHandJoint::LittleDistal},
    {XRHandJoint::LittleDistal, XRHandJoint::LittleTip},
};

constexpr const char* kJointNames[kXRHandJointCount] = {
    "Palm", "Wrist", "ThumbMetacarpal", "ThumbProximal", "ThumbDistal", "ThumbTip",
    "IndexMetacarpal", "IndexProximal", "IndexIntermediate", "IndexDistal", "IndexTip",
    "MiddleMetacarpal", "MiddleProximal", "MiddleIntermediate", "MiddleDistal", "MiddleTip",
    "RingMetacarpal", "RingProximal", "RingIntermediate", "RingDistal", "RingTip",
    "LittleMetacarpal", "LittleProximal", "LittleIntermediate", "LittleDistal", "LittleTip"
};

glm::quat rotatePositiveZTo(const glm::vec3& direction) {
    const glm::vec3 from(0.0f, 0.0f, 1.0f);
    const float cosine = glm::dot(from, direction);
    if (cosine < -0.9999f)
        return glm::angleAxis(glm::pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::vec3 axis = glm::cross(from, direction);
    return glm::normalize(glm::quat(1.0f + cosine, axis.x, axis.y, axis.z));
}

} // namespace

XRHandNode::XRHandNode(saida::XRHand hand)
    : Node(hand == saida::XRHand::Left ? "XR Hand (L)" : "XR Hand (R)"), hand_(hand) {
    if (hand_ == saida::XRHand::Right)
        color_ = {1.0f, 0.45f, 0.25f, 1.0f};
    addBehaviour<XRHandVisualTracker>();
}

bool XRHandNode::isTracked() const {
    return XRInput::handTrackingActive(hand_);
}

void XRHandNode::buildVisuals(ResourceManager& resources) {
    clearChildren();
    jointNodes_.fill(nullptr);
    boneNodes_.clear();

    Mesh* cube = resources.getMesh(kAssetBuiltinCube);
    MaterialDesc materialDesc;
    materialDesc.baseColor = color_;
    materialDesc.metallic = 0.0f;
    materialDesc.roughness = 0.65f;
    Material* material = resources.getMaterial(materialDesc);

    for (int i = 0; i < kXRHandJointCount; ++i) {
        auto joint = std::make_unique<MeshNode>(kJointNames[i], cube, material);
        joint->castShadows() = false;
        joint->setMeshEnabled(false);
        jointNodes_[static_cast<size_t>(i)] = joint.get();
        addChild(std::move(joint));
    }

    boneNodes_.reserve(std::size(kBones));
    for (size_t i = 0; i < std::size(kBones); ++i) {
        auto bone = std::make_unique<MeshNode>("Hand Bone", cube, material);
        bone->castShadows() = false;
        bone->setMeshEnabled(false);
        boneNodes_.push_back(bone.get());
        addChild(std::move(bone));
    }
    visualsBuilt_ = true;
}

void XRHandNode::setVisualsVisible(bool visible) {
    for (MeshNode* joint : jointNodes_)
        if (joint) joint->setMeshEnabled(visible);
    for (MeshNode* bone : boneNodes_)
        if (bone) bone->setMeshEnabled(visible);
}

void XRHandNode::updateTrackingVisuals() {
    if (!visualsBuilt_) return;
    const XRHandSkeletonState& skeleton = XRInput::skeleton(hand_);
    if (!skeleton.active) {
        setVisualsVisible(false);
        return;
    }

    for (int i = 0; i < kXRHandJointCount; ++i) {
        MeshNode* visual = jointNodes_[static_cast<size_t>(i)];
        const XRHandJointState& joint = skeleton.joints[static_cast<size_t>(i)];
        visual->setMeshEnabled(joint.valid);
        if (!joint.valid) continue;
        const float radius = std::clamp(joint.radius * jointScale_, 0.004f, 0.018f);
        visual->transform().position = joint.pose.position;
        visual->transform().rotation = joint.pose.orientation;
        visual->transform().scale = glm::vec3(radius * 2.0f);
    }

    for (size_t i = 0; i < std::size(kBones); ++i) {
        MeshNode* visual = boneNodes_[i];
        const XRHandJointState& from = skeleton.joint(kBones[i].from);
        const XRHandJointState& to = skeleton.joint(kBones[i].to);
        if (!from.valid || !to.valid) {
            visual->setMeshEnabled(false);
            continue;
        }
        const glm::vec3 delta = to.pose.position - from.pose.position;
        const float length = glm::length(delta);
        if (length < 0.0001f) {
            visual->setMeshEnabled(false);
            continue;
        }
        const float thickness = std::clamp(
            std::min(from.radius, to.radius) * 1.35f * jointScale_, 0.003f, 0.012f);
        visual->setMeshEnabled(true);
        visual->transform().position = (from.pose.position + to.pose.position) * 0.5f;
        visual->transform().rotation = rotatePositiveZTo(delta / length);
        visual->transform().scale = {thickness, thickness, length};
    }
}

void XRHandNode::serialize(nlohmann::json& j, ResourceManager& resources) const {
    Node::serialize(j, resources);
    j["hand"] = static_cast<int>(hand_);
    j["color"] = {color_.r, color_.g, color_.b, color_.a};
    j["jointScale"] = jointScale_;
    // Generated visuals are reconstructed from these settings on load.
    j["children"] = nlohmann::json::array();
}

void XRHandNode::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    Node::deserialize(j, resources);
    if (j.contains("hand")) hand_ = static_cast<saida::XRHand>(j["hand"].get<int>());
    if (j.contains("color") && j["color"].is_array() && j["color"].size() == 4)
        color_ = {j["color"][0].get<float>(), j["color"][1].get<float>(),
                  j["color"][2].get<float>(), j["color"][3].get<float>()};
    if (j.contains("jointScale")) jointScale_ = j["jointScale"].get<float>();
    buildVisuals(resources);
}

} // namespace saida
