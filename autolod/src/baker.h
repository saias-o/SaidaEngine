// baker.h - high-poly -> low-poly normal map baking (tangent space)
//
// Re-projects surface details from the high-density mesh (LOD0) onto the
// low-density mesh (LODn) via raycasting, in the UV layout of LODn.
//
// COMPOSITE Bake: if a source normal map is provided, its details (rivets,
// scratches...) are sampled at the hit point, recombined with the high-poly
// geometry, and re-expressed in the tangent space of LODn. The low-poly LOD
// then reproduces the world normals of LOD0 + its normal map.
// Without a source normal map, only geometric normals are baked.
//
// Output: tangent-space normal map (glTF / OpenGL +Y convention) + per-vertex
// tangents for LODn (required to sample the map in the correct basis).

#pragma once
#include <cstddef>
#include <vector>

// High-density source mesh (positions + normals + UV, indexed triangles).
struct BakeHigh {
    const float* pos;          // 3 * vcount
    const float* nrm;          // 3 * vcount
    const float* uv;           // 2 * vcount (to sample the source normal map)
    size_t vcount;
    const unsigned int* idx;
    size_t icount;
};

// Low-density target mesh (positions + normals + UV, indexed triangles).
struct BakeLow {
    const float* pos;          // 3 * vcount
    const float* nrm;          // 3 * vcount
    const float* uv;           // 2 * vcount
    size_t vcount;
    const unsigned int* idx;
    size_t icount;
};

struct BakeResult {
    std::vector<float>         tangents;  // 4 * low.vcount (xyz + w handedness)
    std::vector<unsigned char> png;       // PNG-encoded normal map
    int  width  = 0;
    int  height = 0;
    long long texelsHit = 0;              // diagnostic: texels that hit a triangle
    long long texelsTotal = 0;            // texels covered by UVs
    bool usedSourceMap = false;           // true if source normal map was composited
};

// srcNormalPng / size: PNG/JPG bytes of the source material's normal map
//                      (nullptr -> geometric bake only).
// res       : resolution of the normal map (square)
// cageScale : ray search distance as a fraction of the bbox diagonal
// Returns false if baking is impossible (no UVs, degenerate mesh...).
bool bakeNormalMap(const BakeHigh& high, const BakeLow& low,
                   const unsigned char* srcNormalPng, size_t srcNormalPngSize,
                   int res, float cageScale, BakeResult& out);

// --- Multi-texture baking (atlased proxy LOD) -------------------------------
// Bakes multiple maps in a single pass into LOW's UV layout (typically new
// xatlas UVs). NormalTangent = tangent-space re-projection; Color = direct
// copy (albedo, metallic-roughness, occlusion, emissive).
enum class MapKind { NormalTangent, Color };

struct SrcMap {
    const unsigned char* png = nullptr; // encoded bytes (nullptr ok for NormalTangent -> geo)
    size_t  size = 0;
    MapKind kind = MapKind::Color;
};

struct BakedMap { std::vector<unsigned char> png; };

struct BakeMapsResult {
    std::vector<float>    tangents;       // 4 * low.vcount
    std::vector<BakedMap> maps;           // aligned with sources
    long long texelsHit = 0, texelsTotal = 0;
};

bool bakeMaps(const BakeHigh& high, const BakeLow& low,
              const SrcMap* sources, int nSources,
              int res, float cageScale, BakeMapsResult& out);

