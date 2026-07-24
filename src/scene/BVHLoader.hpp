#pragma once

#include "project/AssetRegistry.hpp"  // AssetID

#include <memory>
#include <string>

namespace saida {

class AnimationClip;
class ResourceManager;

// Loads a Biovision .bvh motion-capture file into an AnimationClip and registers
// it with the ResourceManager (keyed by the file path). Returns the clip's AssetID
// (kAssetInvalid on failure).
//
// The clip's tracks are keyed by joint name, so it can be played on any skeleton
// whose bone names match (e.g. another Mixamo rig) via the Animator — no explicit
// retargeting. Cross-skeleton / cross-convention use (differing rest poses or up
// axes) still needs a proper retargeting pass (not implemented yet).
class BVHLoader {
public:
    static AssetID load(const std::string& path, ResourceManager& resources);

    // Parse without registering (no GPU, no ResourceManager) — tests and tooling.
    // Null on failure.
    static std::unique_ptr<AnimationClip> parse(const std::string& path);
};

} // namespace saida
