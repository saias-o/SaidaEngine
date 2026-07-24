#pragma once

#include "project/AssetLoader.hpp"
#include "scene/animation/AnimGraphAsset.hpp"
#include "scene/animation/ClipView.hpp"
#include "scene/animation/RigAsset.hpp"

#include <vector>

namespace saida {

// CPU payloads produced by the AssetLoader. Diagnostics are kept around
// until the main thread so ResourceManager can log them without the
// worker touching engine state.
struct DecodedRigAsset {
    RigAsset asset;
    std::vector<AssetDiagnostic> diagnostics;
};

struct DecodedClipView {
    ClipView view;
    std::vector<AssetDiagnostic> diagnostics;
};

struct DecodedAnimGraph {
    AnimGraphAsset graph;
    std::vector<AssetDiagnostic> diagnostics;
};

AssetDecoder makeRigAssetDecoder();
AssetDecoder makeClipViewDecoder();
AssetDecoder makeAnimGraphDecoder();

} // namespace saida
