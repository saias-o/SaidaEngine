#include "core/Paths.hpp"
#include "core/Window.hpp"
#include "graphics/ResourceManager.hpp"
#include "graphics/VulkanDevice.hpp"
#include "nodes/MeshNode.hpp"
#include "scene/GLTFLoader.hpp"
#include "scene/animation/AnimationClip.hpp"
#include "scene/animation/Animator.hpp"
#include "xr/toolkit/XRHand.hpp"
#include "xr/toolkit/XRHandPose.hpp"
#include "xr/toolkit/XRInput.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <iostream>

using namespace saida;

namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "[xr-hand] FAIL: " << message << '\n'; std::abort(); }
}
bool near(const glm::mat4& a, const glm::mat4& b) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(a[c][r] - b[c][r]) > 2e-4f) return false;
    return true;
}

XRHandSkeletonState makeSkeleton(const Rig& rig, const glm::mat4& world) {
    LocalPose rest;
    for (const auto& bone : rig.bones()) rest.localTransforms.push_back(bone.restLocal);
    GlobalPose global;
    global.computeFrom(rest, rig);
    XRHandSkeletonState skeleton;
    skeleton.active = true;
    const char* names[] = {"Palm", "Hand", "ThumbMetacarpal", "ThumbProximal", "ThumbDistal", "ThumbTip",
        "IndexMetacarpal", "IndexProximal", "IndexIntermediate", "IndexDistal", "IndexTip",
        "MiddleMetacarpal", "MiddleProximal", "MiddleIntermediate", "MiddleDistal", "MiddleTip",
        "RingMetacarpal", "RingProximal", "RingIntermediate", "RingDistal", "RingTip",
        "LittleMetacarpal", "LittleProximal", "LittleIntermediate", "LittleDistal", "LittleTip"};
    const std::string prefix = rig.bones()[0].name.substr(0, rig.bones()[0].name.find("Hand"));
    for (int j = 1; j < kXRHandJointCount; ++j) {
        const int i = rig.findBoneIndex(prefix + names[j]);
        require(i >= 0, "all tracked joints map to the downloaded rig");
        const glm::mat4 tracked = world * global.globalMatrices[static_cast<size_t>(i)]
            * glm::mat4_cast(glm::inverse(xrHandHumanoidAxes()));
        skeleton.joints[j].valid = true;
        skeleton.joints[j].pose.position = glm::vec3(tracked[3]);
        skeleton.joints[j].pose.orientation = glm::quat_cast(tracked);
    }
    return skeleton;
}

void testPose(XRHand side) {
    const auto axes = xrHandHumanoidAxes();
    require(glm::length(axes * glm::vec3(0, 1, 0) - glm::vec3(0, 0, -1)) < 1e-5f,
            "humanoid finger axis points down OpenXR -Z");
    require(glm::length(axes * glm::vec3(0, 0, -1) - glm::vec3(0, 1, 0)) < 1e-5f,
            "humanoid dorsal axis points along OpenXR +Y");
    GltfAnimationData data;
    std::string error;
    require(GLTFLoader::loadAnimationData(assetPath(side == XRHand::Left
        ? "assets/xr/hands/LeftHand.glb" : "assets/xr/hands/RightHand.glb"), data, &error), "GLB loads");
    require(data.rigs.size() == 1, "one skin per hand");
    const Rig& rig = *data.rigs[0];
    XRHandPose solver(rig, side);
    require(solver.valid(), "downloaded hand has the supported 25-bone rig");
    const glm::mat4 world = glm::translate(glm::mat4(1), {4, 2, -7})
        * glm::mat4_cast(glm::angleAxis(0.8f, glm::vec3(0, 1, 0)));
    auto skeleton = makeSkeleton(rig, world);
    LocalPose output;
    solver.evaluate(skeleton, world, output);
    GlobalPose global;
    global.computeFrom(output, rig);
    for (size_t i = 0; i < rig.boneCount(); ++i)
        require(near(global.skinningMatrices[i], glm::mat4(1)), "rest pose preserves mesh under a translated/rotated parent");

    // Move and rotate an individual distal joint; its skin must follow in this
    // frame, including the fingertip child, with no change to the other hand.
    auto& distal = skeleton.joints[static_cast<int>(XRHandJoint::IndexDistal)];
    distal.pose.position += glm::vec3(0.01f, 0.02f, -0.01f);
    distal.pose.orientation = glm::angleAxis(0.5f, glm::vec3(1, 0, 0)) * distal.pose.orientation;
    solver.evaluate(skeleton, world, output);
    global.computeFrom(output, rig);
    const int index = rig.findBoneIndex(side == XRHand::Left ? "LeftIndexDistal" : "RightIndexDistal");
    glm::mat4 expected = glm::mat4_cast(distal.pose.orientation);
    expected[3] = glm::vec4(distal.pose.position, 1);
    require(near(world * global.globalMatrices[index], expected * glm::mat4_cast(xrHandHumanoidAxes())), "finger follows the tracked world pose");
    distal.valid = false;
    solver.evaluate(skeleton, world, output);
    require(near(output.localTransforms[index].matrix(), rig.bones()[index].restLocal.matrix()), "invalid joint uses its rest pose");
}

void testVisuals() {
    Window window(64, 64, "XR hand verification", false);
    VulkanDevice device(window);
    ResourceManager resources(device);
    XRInput::clearHand(XRHand::Left);
    XRInput::clearHand(XRHand::Right);
    XRHandNode hand;
    hand.buildVisuals(resources);
    MeshNode* mesh = nullptr;
    hand.traverse([&](Node& node, const glm::mat4&) {
        if (auto* m = dynamic_cast<MeshNode*>(&node)) {
            require(mesh == nullptr, "default hand is one skinned mesh, without debug cubes");
            mesh = m;
        }
    });
    require(mesh && mesh->getBehaviour<Animator>(), "default GLB is connected to the animator");
    auto* animator = mesh->getBehaviour<Animator>();
    require(!mesh->meshEnabled(), "hand starts hidden");
    auto skeleton = makeSkeleton(*animator->rig(), glm::mat4(1));
    XRInput::submitSkeleton(XRHand::Left, skeleton);
    hand.updateTree(1.0f / 90);
    require(mesh->meshEnabled(), "active tracking reveals the skin");
    require(animator->globalPose().skinningMatrices.size() == 25, "tracking evaluates the skin in the same update");
    skeleton.joints[static_cast<int>(XRHandJoint::Wrist)].valid = false;
    XRInput::submitSkeleton(XRHand::Left, skeleton);
    hand.updateTree(1.0f / 90);
    require(!mesh->meshEnabled(), "invalid wrist hides the skin");
    skeleton.active = false;
    XRInput::submitSkeleton(XRHand::Left, skeleton);
    hand.updateTree(1.0f / 90);
    require(!mesh->meshEnabled(), "tracking loss hides the skin");
    nlohmann::json serialized;
    hand.serialize(serialized, resources);
    require(serialized["children"].empty(), "generated visuals are not serialized");
    XRHandNode restored;
    restored.deserialize(serialized, resources);
    restored.setHand(XRHand::Right);
    bool right = false;
    restored.traverse([&](Node& node, const glm::mat4&) {
        if (auto* a = node.getBehaviour<Animator>()) right = a->rig()->findBoneIndex("RightHand") >= 0;
    });
    require(right, "changing handedness rebuilds the correct model");
    const std::string oldRoot = runtimeRoot();
    setRuntimeRoot(assetPath("build-rel/tmp/missing-xr-hand-assets"));
    XRHandNode fallback;
    fallback.buildVisuals(resources);
    size_t cubes = 0;
    fallback.traverse([&](Node& node, const glm::mat4&) {
        if (auto* m = dynamic_cast<MeshNode*>(&node)) {
            require(!m->getBehaviour<Animator>(), "missing GLB uses procedural meshes");
            ++cubes;
        }
    });
    require(cubes > 26, "missing GLB retains joint and bone fallback");
    setRuntimeRoot(oldRoot);
    XRInput::submitSkeleton(XRHand::Left, {});
    XRInput::submitSkeleton(XRHand::Right, {});
    XRInput::clearHand(XRHand::Left);
    XRInput::clearHand(XRHand::Right);
}
}

int main(int argc, char** argv) {
    testPose(XRHand::Left);
    testPose(XRHand::Right);
    if (argc == 2 && std::string(argv[1]) == "--pose-only") return 0;
    testVisuals();
    std::cout << "[xr-hand] pose and default GLB integration passed\n";
}
