#pragma once

#include <memory>
#include <string>
#include <vector>

namespace saida {

class Node;
class ResourceManager;
class Rig;
class AnimationClip;

struct GLTFLoadOptions {
    bool autoMeshLods = false;
};

// Rigs and clips of a glTF file, without the render-side assets (meshes,
// textures, nodes). Loadable headless — tests and tooling use this path.
struct GltfAnimationData {
    std::vector<std::unique_ptr<Rig>> rigs;              // one per skin
    std::vector<std::unique_ptr<AnimationClip>> clips;   // one per animation
    std::vector<std::string> clipNames;
};

class GLTFLoader {
public:
    static bool load(const std::string& path, Node& rootNode, ResourceManager& resources,
                     const GLTFLoadOptions& options = {});

    // Animation-only import (no GPU, no ResourceManager). Same construction code
    // as load(), so a pose golden-tested here is the pose the editor plays.
    static bool loadAnimationData(const std::string& path, GltfAnimationData& out,
                                  std::string* error = nullptr);
};

} // namespace saida
