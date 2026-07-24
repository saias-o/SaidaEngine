// baker.h - bake de normal map haute-poly -> basse-poly (espace tangent)
//
// Re-projette le detail de surface du mesh haute densite (LOD0) sur le mesh
// basse densite (LODn) via raycasting, dans le layout UV du LODn.
//
// Bake COMPOSITE : si une normal map source est fournie, son detail (rivets,
// rayures...) est echantillonne au point touche puis recombine avec la
// geometrie haute-poly et re-exprime dans la base tangente du LODn. Le LOD
// basse-poly reproduit alors les normales monde du LOD0 + sa normal map.
// Sans normal map source, on bake uniquement les normales geometriques.
//
// Sortie : normal map tangent-space (convention glTF / OpenGL +Y) + tangentes
// par sommet du LODn (necessaires pour lire la map dans la bonne base).

#pragma once
#include <cstddef>
#include <vector>

// Mesh source haute densite (positions + normales + UV, indexe triangles).
struct BakeHigh {
    const float* pos;          // 3 * vcount
    const float* nrm;          // 3 * vcount
    const float* uv;           // 2 * vcount (pour echantillonner la normal map source)
    size_t vcount;
    const unsigned int* idx;
    size_t icount;
};

// Mesh cible basse densite (positions + normales + UV, indexe triangles).
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
    std::vector<unsigned char> png;       // normal map encodee en PNG
    int  width  = 0;
    int  height = 0;
    long long texelsHit = 0;              // diagnostic : texels ayant trouve un hit
    long long texelsTotal = 0;            // texels couverts par les UV
    bool usedSourceMap = false;           // true si la normal map source a ete compositee
};

// srcNormalPng / size : octets PNG/JPG de la normal map du materiau source
//                       (nullptr -> bake geometrique seul).
// res        : resolution de la normal map (carree)
// cageScale  : distance de recherche du ray, en fraction de la diagonale bbox
// Retourne false si le bake est impossible (pas d'UV, mesh degenere...).
bool bakeNormalMap(const BakeHigh& high, const BakeLow& low,
                   const unsigned char* srcNormalPng, size_t srcNormalPngSize,
                   int res, float cageScale, BakeResult& out);

// --- Bake multi-textures (proxy LOD atlase) ---------------------------------
// Bake plusieurs maps en une passe vers le layout UV du LOW (typiquement de
// nouvelles UV xatlas). NormalTangent = re-projection tangent-space ; Color =
// copie directe (albedo, metallic-roughness, occlusion, emissive).
enum class MapKind { NormalTangent, Color };

struct SrcMap {
    const unsigned char* png = nullptr; // octets encodes (nullptr ok pour NormalTangent -> geo)
    size_t  size = 0;
    MapKind kind = MapKind::Color;
};

struct BakedMap { std::vector<unsigned char> png; };

struct BakeMapsResult {
    std::vector<float>    tangents;       // 4 * low.vcount
    std::vector<BakedMap> maps;           // aligne avec les sources
    long long texelsHit = 0, texelsTotal = 0;
};

bool bakeMaps(const BakeHigh& high, const BakeLow& low,
              const SrcMap* sources, int nSources,
              int res, float cageScale, BakeMapsResult& out);
