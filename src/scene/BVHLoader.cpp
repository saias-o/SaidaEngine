#include "scene/BVHLoader.hpp"

#include "core/Profiler.hpp"
#include "scene/animation/AnimationClip.hpp"
#include "graphics/ResourceManager.hpp"
#include "core/Log.hpp"

#include <glm/gtc/quaternion.hpp>

#include <fstream>
#include <sstream>
#include <vector>
#include <string>

namespace saida {

namespace {

enum class Chan { Xpos, Ypos, Zpos, Xrot, Yrot, Zrot };

struct BvhJoint {
    std::string name;
    std::vector<Chan> channels;  // in file order (matters for rotation composition)
    bool hasPosition = false;
};

Chan parseChannel(const std::string& s) {
    if (s == "Xposition") return Chan::Xpos;
    if (s == "Yposition") return Chan::Ypos;
    if (s == "Zposition") return Chan::Zpos;
    if (s == "Xrotation") return Chan::Xrot;
    if (s == "Yrotation") return Chan::Yrot;
    return Chan::Zrot;  // Zrotation
}

} // namespace

std::unique_ptr<AnimationClip> BVHLoader::parse(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        Log::error("BVHLoader: cannot open ", path);
        return nullptr;
    }

    std::vector<BvhJoint> joints;
    int totalChannels = 0;

    std::string tok;
    bool inEndSite = false;
    while (file >> tok) {
        if (tok == "MOTION") break;
        if (tok == "ROOT" || tok == "JOINT") {
            std::string name;
            file >> name;
            joints.push_back({name, {}, false});
        } else if (tok == "End") {
            file >> tok;     // "Site"
            inEndSite = true;  // End Site has an OFFSET but no channels — skip it
        } else if (tok == "}") {
            inEndSite = false;
        } else if (tok == "CHANNELS" && !inEndSite && !joints.empty()) {
            int n = 0;
            file >> n;
            for (int i = 0; i < n; ++i) {
                std::string c;
                file >> c;
                Chan ch = parseChannel(c);
                joints.back().channels.push_back(ch);
                if (ch == Chan::Xpos || ch == Chan::Ypos || ch == Chan::Zpos)
                    joints.back().hasPosition = true;
            }
            totalChannels += n;
        }
        // OFFSET values are not needed for the clip (the target rig provides bone
        // offsets); they are consumed harmlessly by the token loop.
    }

    if (joints.empty() || totalChannels == 0) {
        Log::error("BVHLoader: no joints/channels in ", path);
        return nullptr;
    }

    int frameCount = 0;
    float frameTime = 1.0f / 30.0f;
    file >> tok;  // "Frames:"
    file >> frameCount;
    file >> tok >> tok;  // "Frame" "Time:"
    file >> frameTime;

    if (frameCount <= 0) {
        Log::error("BVHLoader: no frames in ", path);
        return nullptr;
    }

    // Per-joint keyframe accumulators.
    std::vector<std::vector<glm::vec3>> positions(joints.size());
    std::vector<std::vector<glm::quat>> rotations(joints.size());
    std::vector<float> timestamps(frameCount);

    for (int f = 0; f < frameCount; ++f) {
        timestamps[f] = f * frameTime;
        for (size_t j = 0; j < joints.size(); ++j) {
            glm::vec3 pos(0.0f);
            glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
            for (Chan ch : joints[j].channels) {
                float v = 0.0f;
                if (!(file >> v)) {
                    Log::error("BVHLoader: truncated motion data in ", path);
                    return nullptr;
                }
                switch (ch) {
                    case Chan::Xpos: pos.x = v; break;
                    case Chan::Ypos: pos.y = v; break;
                    case Chan::Zpos: pos.z = v; break;
                    // Compose in file order (degrees → radians).
                    case Chan::Xrot: rot = rot * glm::angleAxis(glm::radians(v), glm::vec3(1, 0, 0)); break;
                    case Chan::Yrot: rot = rot * glm::angleAxis(glm::radians(v), glm::vec3(0, 1, 0)); break;
                    case Chan::Zrot: rot = rot * glm::angleAxis(glm::radians(v), glm::vec3(0, 0, 1)); break;
                }
            }
            if (joints[j].hasPosition) positions[j].push_back(pos);
            rotations[j].push_back(rot);
        }
    }

    float duration = (frameCount - 1) * frameTime;
    auto clip = std::make_unique<AnimationClip>(path, duration);

    for (size_t j = 0; j < joints.size(); ++j) {
        if (joints[j].hasPosition) {
            auto track = std::make_unique<TypedAnimTrack<glm::vec3>>();
            track->target = TrackTarget::Translation;
            track->timestamps = timestamps;
            track->values = std::move(positions[j]);
            clip->addTrack(joints[j].name, std::move(track));
        }
        auto rtrack = std::make_unique<TypedAnimTrack<glm::quat>>();
        rtrack->target = TrackTarget::Rotation;
        rtrack->timestamps = timestamps;
        rtrack->values = std::move(rotations[j]);
        clip->addTrack(joints[j].name, std::move(rtrack));
    }

    Log::info("Parsed BVH '", path, "': ", joints.size(), " joints, ", frameCount,
              " frames, duration=", duration, "s");
    return clip;
}

AssetID BVHLoader::load(const std::string& path, ResourceManager& resources) {
    SAIDA_PROFILE_SCOPE("Resource/LoadBVH");
    auto clip = parse(path);
    if (!clip) return kAssetInvalid;
    return resources.registerMemoryAnimation(path, std::move(clip));
}

} // namespace saida
