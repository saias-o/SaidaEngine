#pragma once

#include <string>

namespace saida {

// Offline bridge to the AutoLOD tool (autolod.exe). When enabled in project
// settings, GLB imports run through AutoLOD first to produce a *_lod.glb asset.
class AutoLODBridge {
public:
    // Absolute path to autolod.exe, or empty if not built/found.
    static std::string exePath();

    // Run autolod on inputGlb → outputGlb. Returns true if output exists afterward.
    static bool generate(const std::string& inputGlb, std::string& outputGlb);

    // If autoGenerate is true and path is a .glb, returns the LOD-enriched path
    // (generating it when needed). Otherwise returns path unchanged.
    static std::string resolveLoadPath(const std::string& path, bool autoGenerate);
};

} // namespace saida
