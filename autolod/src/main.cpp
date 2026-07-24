// AutoLOD - generateur de LOD hors-ligne pour meshs statiques glTF/GLB
//
// Entree  : un .gltf ou .glb (rocher, batiment, prop...)
// Sortie  : le meme asset enrichi de LOD1..N (extension MSFT_lod + nodes nommes)
//
// Pipeline : tinygltf (IO) -> weld -> meshoptimizer (simplify + sloppy fallback)
//            -> reinjection des index buffers -> ecriture.
//
// Licence du code : MIT. Depend de meshoptimizer (MIT) et tinygltf (MIT).

#define TINYGLTF_IMPLEMENTATION
// On ne decode pas les pixels : les textures sont preservees telles quelles
// (bufferView ou URI) en pass-through. Evite stb et allege la compilation.
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#include "tiny_gltf.h"

#include <meshoptimizer.h>
#include <xatlas.h>

#include "baker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using tinygltf::Model;
using tinygltf::Mesh;
using tinygltf::Primitive;
using tinygltf::Accessor;
using tinygltf::BufferView;
using tinygltf::Buffer;
using tinygltf::Node;

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
struct LodLevel {
    float ratio;       // fraction d'index a conserver (1.0 = LOD0)
    float error;       // erreur relative max toleree (0.01 = 1% de la taille du mesh)
    bool  allowSloppy; // autorise la decimation "sloppy" si le QEM n'atteint pas la cible
    float coverage;    // seuil de couverture ecran (MSFT_screencoverage)
    float uvWeight;    // poids QEM des UV pour CE niveau : plus eleve = moins de distorsion texture
                       // Pour les LOD tres aggressifs, augmenter fortement (8-16) oblige l'algo
                       // a rejeter les effondrements d'aretes qui etireraient les UV.
};

struct Config {
    std::vector<LodLevel> levels = {
        // ratio  error  sloppy  coverage  uvWeight
        { 0.60f, 0.010f, false, 0.50f,  1.0f }, // LOD1 : leger, UV quasi-intact
        { 0.30f, 0.020f, false, 0.25f,  3.0f }, // LOD2 : modere
        { 0.10f, 0.050f, false, 0.08f, 10.0f }, // LOD3 : agressif -> UV ultra-preserves
    };
    float normalWeight = 0.5f; // poids QEM des normales (global)
    bool  lockBorder   = false; // verrouille les bords ouverts (assets modulaires emboitables)
    // Decimation "sloppy" : ignore TOTALEMENT les UV (casse les textures). Off par
    // defaut ; utile seulement pour des LOD tres lointains sans dependance texture.
    bool  allowSloppy  = false;

    // --- Baking de normal map (re-projection du detail haute-poly) ---
    bool  bake      = false;   // active le bake (--bake)
    int   bakeRes   = 1024;    // resolution de la normal map (--bake-res)
    float bakeCage  = 0.03f;   // distance de recherche en fraction de la diag bbox
    float bakeMinRatio = 0.40f;// ne bake que les LOD dont ratio <= ce seuil (LOD2, LOD3...)

    // --- Proxy LOD atlase (re-depliage xatlas + bake de TOUTES les textures) ---
    bool  proxy        = false; // active le mode proxy (--proxy), implique le bake
    float proxyBelow   = 0.15f; // les LOD de ratio < ce seuil deviennent des proxy
};

// ---------------------------------------------------------------------------
// Lecteurs d'accessors (gerent les types/strides courants)
// ---------------------------------------------------------------------------
static std::vector<uint32_t> readIndices(const Model& m, const Primitive& prim, size_t vertexCount) {
    std::vector<uint32_t> out;
    if (prim.indices < 0) { // mesh non indexe -> 0,1,2,...
        out.resize(vertexCount);
        for (size_t i = 0; i < vertexCount; ++i) out[i] = static_cast<uint32_t>(i);
        return out;
    }
    const Accessor& a   = m.accessors[prim.indices];
    const BufferView& bv = m.bufferViews[a.bufferView];
    const Buffer& b     = m.buffers[bv.buffer];
    const unsigned char* base = b.data.data() + bv.byteOffset + a.byteOffset;
    const int cs = tinygltf::GetComponentSizeInBytes(a.componentType);
    const size_t stride = bv.byteStride ? bv.byteStride : static_cast<size_t>(cs);
    out.resize(a.count);
    for (size_t i = 0; i < a.count; ++i) {
        const unsigned char* p = base + i * stride;
        switch (a.componentType) {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:   { uint32_t v; std::memcpy(&v, p, 4); out[i] = v; } break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: { uint16_t v; std::memcpy(&v, p, 2); out[i] = v; } break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  { out[i] = *p; } break;
            default: out[i] = 0; break;
        }
    }
    return out;
}

// Lit un attribut vers un buffer flottant tightly-packed (count * comps).
static std::vector<float> readFloats(const Model& m, int accIdx, int comps) {
    const Accessor& a    = m.accessors[accIdx];
    const BufferView& bv = m.bufferViews[a.bufferView];
    const Buffer& b      = m.buffers[bv.buffer];
    const unsigned char* base = b.data.data() + bv.byteOffset + a.byteOffset;
    const int cs   = tinygltf::GetComponentSizeInBytes(a.componentType);
    const int ncmp = tinygltf::GetNumComponentsInType(a.type);
    const size_t stride = bv.byteStride ? bv.byteStride : static_cast<size_t>(cs) * ncmp;
    std::vector<float> out(static_cast<size_t>(a.count) * comps, 0.0f);
    for (size_t i = 0; i < a.count; ++i) {
        const unsigned char* p = base + i * stride;
        for (int c = 0; c < comps && c < ncmp; ++c) {
            const unsigned char* pc = p + c * cs;
            float v = 0.0f;
            switch (a.componentType) {
                case TINYGLTF_COMPONENT_TYPE_FLOAT:          { std::memcpy(&v, pc, 4); } break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: { uint16_t t; std::memcpy(&t, pc, 2); v = a.normalized ? t / 65535.0f : float(t); } break;
                case TINYGLTF_COMPONENT_TYPE_SHORT:          { int16_t t;  std::memcpy(&t, pc, 2); v = a.normalized ? std::max(t / 32767.0f, -1.0f) : float(t); } break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  { v = a.normalized ? *pc / 255.0f : float(*pc); } break;
                case TINYGLTF_COMPONENT_TYPE_BYTE:           { int8_t t = *reinterpret_cast<const int8_t*>(pc); v = a.normalized ? std::max(t / 127.0f, -1.0f) : float(t); } break;
                default: break;
            }
            out[i * comps + c] = v;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Helpers d'ecriture dans buffer[0]
// ---------------------------------------------------------------------------
static size_t appendData(Model& m, const void* ptr, size_t bytes, size_t align = 4) {
    auto& d = m.buffers[0].data;
    while (d.size() % align != 0) d.push_back(0);
    const size_t off = d.size();
    const unsigned char* s = static_cast<const unsigned char*>(ptr);
    d.insert(d.end(), s, s + bytes);
    return off;
}

static int makeBufferView(Model& m, size_t off, size_t len, size_t stride, int target) {
    BufferView bv;
    bv.buffer     = 0;
    bv.byteOffset = off;
    bv.byteLength = len;
    bv.byteStride = stride; // 0 = tightly packed / indices
    bv.target     = target;
    m.bufferViews.push_back(bv);
    return static_cast<int>(m.bufferViews.size() - 1);
}

static int makeAccessor(Model& m, int bufferView, size_t byteOffset, int componentType,
                        int type, size_t count,
                        const std::vector<double>* mn = nullptr,
                        const std::vector<double>* mx = nullptr) {
    Accessor a;
    a.bufferView    = bufferView;
    a.byteOffset    = byteOffset;
    a.componentType = componentType;
    a.count         = count;
    a.type          = type;
    a.normalized    = false;
    if (mn) a.minValues = *mn;
    if (mx) a.maxValues = *mx;
    m.accessors.push_back(a);
    return static_cast<int>(m.accessors.size() - 1);
}

// Embarque un PNG (normal map) + cree image/sampler/texture -> index texture.
static int addPngTexture(Model& m, const std::vector<unsigned char>& png) {
    const size_t off = appendData(m, png.data(), png.size(), 4);
    const int bv = makeBufferView(m, off, png.size(), 0, 0); // pas de target pour une image
    tinygltf::Image img;
    img.mimeType   = "image/png";
    img.bufferView = bv;
    img.as_is      = true;
    m.images.push_back(img);
    const int imgIdx = static_cast<int>(m.images.size() - 1);

    tinygltf::Sampler s;
    s.minFilter = TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR;
    s.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
    s.wrapS = TINYGLTF_TEXTURE_WRAP_REPEAT;
    s.wrapT = TINYGLTF_TEXTURE_WRAP_REPEAT;
    m.samplers.push_back(s);
    const int sampIdx = static_cast<int>(m.samplers.size() - 1);

    tinygltf::Texture tex;
    tex.source  = imgIdx;
    tex.sampler = sampIdx;
    m.textures.push_back(tex);
    return static_cast<int>(m.textures.size() - 1);
}

// Clone un materiau et remplace sa normalTexture par la map bakee -> index materiau.
static int cloneMaterialWithNormal(Model& m, int srcMat, int normalTex, const std::string& suffix) {
    tinygltf::Material mat;
    if (srcMat >= 0 && srcMat < (int)m.materials.size()) mat = m.materials[srcMat];
    mat.name = (mat.name.empty() ? "lod_mat" : mat.name) + suffix;
    mat.normalTexture.index    = normalTex;
    mat.normalTexture.texCoord = 0;
    mat.normalTexture.scale    = 1.0;
    m.materials.push_back(mat);
    return static_cast<int>(m.materials.size() - 1);
}

// Octets encodes (PNG/JPG) de l'image d'une texture, via le loader as_is.
static std::vector<unsigned char> textureBytes(const Model& m, int texIdx) {
    if (texIdx < 0 || texIdx >= (int)m.textures.size()) return {};
    const int img = m.textures[texIdx].source;
    if (img < 0 || img >= (int)m.images.size()) return {};
    return m.images[img].image;
}

// Re-depliage UV d'un mesh decime via xatlas. Produit un nouveau mesh (les
// sommets peuvent etre dupliques le long des coutures) avec des UV propres [0,1].
struct Unwrapped {
    std::vector<float> pos, nrm, uv;        // 3*, 3*, 2*
    std::vector<unsigned int> idx;
    size_t vcount = 0;
};
// Normales lisses (ponderees par aire) recalculees sur un maillage decime.
static std::vector<float> computeSmoothNormals(const std::vector<float>& pos,
                                               const std::vector<unsigned int>& idx) {
    const size_t vc = pos.size() / 3;
    std::vector<float> nrm(vc * 3, 0.0f);
    auto P = [&](unsigned int i, int c){ return pos[i*3+c]; };
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        const unsigned int a=idx[t], b=idx[t+1], c=idx[t+2];
        const float e1x=P(b,0)-P(a,0), e1y=P(b,1)-P(a,1), e1z=P(b,2)-P(a,2);
        const float e2x=P(c,0)-P(a,0), e2y=P(c,1)-P(a,1), e2z=P(c,2)-P(a,2);
        const float nx=e1y*e2z-e1z*e2y, ny=e1z*e2x-e1x*e2z, nz=e1x*e2y-e1y*e2x; // = 2*aire*normale
        for (unsigned int v : { a,b,c }) { nrm[v*3+0]+=nx; nrm[v*3+1]+=ny; nrm[v*3+2]+=nz; }
    }
    for (size_t v = 0; v < vc; ++v) {
        const float l = std::sqrt(nrm[v*3]*nrm[v*3]+nrm[v*3+1]*nrm[v*3+1]+nrm[v*3+2]*nrm[v*3+2]);
        if (l > 1e-20f) { nrm[v*3]/=l; nrm[v*3+1]/=l; nrm[v*3+2]/=l; } else nrm[v*3+2]=1.0f;
    }
    return nrm;
}

static bool xatlasUnwrap(const std::vector<float>& pos, const std::vector<float>& nrm,
                         const std::vector<unsigned int>& idx, int res, Unwrapped& out) {
    xatlas::Atlas* atlas = xatlas::Create();
    xatlas::MeshDecl md;
    md.vertexCount          = (uint32_t)(pos.size() / 3);
    md.vertexPositionData   = pos.data();
    md.vertexPositionStride = 3 * sizeof(float);
    md.vertexNormalData     = nrm.data();
    md.vertexNormalStride   = 3 * sizeof(float);
    md.indexCount           = (uint32_t)idx.size();
    md.indexData            = idx.data();
    md.indexFormat          = xatlas::IndexFormat::UInt32;
    if (xatlas::AddMesh(atlas, md) != xatlas::AddMeshError::Success) { xatlas::Destroy(atlas); return false; }

    xatlas::ChartOptions co;
    xatlas::PackOptions po;
    po.resolution = res;
    po.padding    = 2;
    po.bilinear   = true;
    xatlas::Generate(atlas, co, po);

    if (atlas->meshCount == 0 || atlas->width == 0 || atlas->height == 0) { xatlas::Destroy(atlas); return false; }
    const xatlas::Mesh& om = atlas->meshes[0];
    out.vcount = om.vertexCount;
    out.pos.resize(om.vertexCount * 3);
    out.nrm.resize(om.vertexCount * 3);
    out.uv.resize (om.vertexCount * 2);
    for (uint32_t v = 0; v < om.vertexCount; ++v) {
        const xatlas::Vertex& vv = om.vertexArray[v];
        const uint32_t r = vv.xref;
        out.pos[v*3+0]=pos[r*3+0]; out.pos[v*3+1]=pos[r*3+1]; out.pos[v*3+2]=pos[r*3+2];
        out.nrm[v*3+0]=nrm[r*3+0]; out.nrm[v*3+1]=nrm[r*3+1]; out.nrm[v*3+2]=nrm[r*3+2];
        out.uv[v*2+0] = vv.uv[0] / atlas->width;
        out.uv[v*2+1] = vv.uv[1] / atlas->height;
    }
    out.idx.assign(om.indexArray, om.indexArray + om.indexCount);
    xatlas::Destroy(atlas);
    return out.vcount >= 3 && out.idx.size() >= 3;
}

// ---------------------------------------------------------------------------
// Generation des meshs LOD pour un mesh source -> indices des nouveaux meshs
// ---------------------------------------------------------------------------
static std::vector<int> generateLodMeshes(Model& model, int meshIndex, const Config& cfg) {
    const size_t numLods = cfg.levels.size();
    std::vector<std::vector<Primitive>> lodPrims(numLods);

    const Mesh src = model.meshes[meshIndex]; // copie : on va push_back dans model.*

    for (const Primitive& prim : src.primitives) {
        if (prim.mode != TINYGLTF_MODE_TRIANGLES && prim.mode != -1) continue;
        auto itPos = prim.attributes.find("POSITION");
        if (itPos == prim.attributes.end()) continue;

        const int posAcc = itPos->second;
        auto itN = prim.attributes.find("NORMAL");
        auto itT = prim.attributes.find("TEXCOORD_0");
        const int nrmAcc = (itN != prim.attributes.end()) ? itN->second : -1;
        const int uvAcc  = (itT != prim.attributes.end()) ? itT->second : -1;

        std::vector<float> pos = readFloats(model, posAcc, 3);
        const size_t vcount = pos.size() / 3;
        if (vcount == 0) continue;
        std::vector<float> nrm = (nrmAcc >= 0) ? readFloats(model, nrmAcc, 3) : std::vector<float>();
        std::vector<float> uv  = (uvAcc  >= 0) ? readFloats(model, uvAcc, 2)  : std::vector<float>();
        std::vector<uint32_t> idx = readIndices(model, prim, vcount);
        if (idx.size() < 3) continue;

        // Interleave : [pos(3)][nrm(3)?][uv(2)?]
        const bool hasN = nrmAcc >= 0;
        const bool hasUV = uvAcc >= 0;
        const int comps = 3 + (hasN ? 3 : 0) + (hasUV ? 2 : 0);
        std::vector<float> vtx(vcount * comps);
        for (size_t i = 0; i < vcount; ++i) {
            int o = 0;
            vtx[i * comps + o++] = pos[i * 3 + 0];
            vtx[i * comps + o++] = pos[i * 3 + 1];
            vtx[i * comps + o++] = pos[i * 3 + 2];
            if (hasN)  { vtx[i * comps + o++] = nrm[i * 3 + 0]; vtx[i * comps + o++] = nrm[i * 3 + 1]; vtx[i * comps + o++] = nrm[i * 3 + 2]; }
            if (hasUV) { vtx[i * comps + o++] = uv[i * 2 + 0];  vtx[i * comps + o++] = uv[i * 2 + 1]; }
        }

        // Weld : indispensable, les exports dupliquent souvent les sommets par triangle.
        const size_t vstride = static_cast<size_t>(comps) * sizeof(float);
        std::vector<unsigned int> remap(vcount);
        const size_t uniqueCount = meshopt_generateVertexRemap(
            remap.data(), idx.data(), idx.size(), vtx.data(), vcount, vstride);
        std::vector<float> wvtx(uniqueCount * comps);
        std::vector<unsigned int> widx(idx.size());
        meshopt_remapVertexBuffer(wvtx.data(), vtx.data(), vcount, vstride, remap.data());
        meshopt_remapIndexBuffer(widx.data(), idx.data(), idx.size(), remap.data());

        const float scale = meshopt_simplifyScale(wvtx.data(), uniqueCount, vstride);

        // High-poly deinterleave (positions + normales + UV) pour le baker, une fois.
        const bool canBake = cfg.bake && hasN && hasUV;
        std::vector<float> hpPos, hpNrm, hpUv;
        // Octets des textures source par canal (pour le proxy : on bake tout)
        std::vector<unsigned char> srcNormalPng, srcColorPng, srcMrPng, srcAoPng, srcEmiPng;
        if (canBake) {
            hpPos.resize(uniqueCount * 3);
            hpNrm.resize(uniqueCount * 3);
            hpUv.resize(uniqueCount * 2);
            for (size_t i = 0; i < uniqueCount; ++i) {
                hpPos[i*3+0] = wvtx[i*comps+0]; hpPos[i*3+1] = wvtx[i*comps+1]; hpPos[i*3+2] = wvtx[i*comps+2];
                hpNrm[i*3+0] = wvtx[i*comps+3]; hpNrm[i*3+1] = wvtx[i*comps+4]; hpNrm[i*3+2] = wvtx[i*comps+5];
                hpUv[i*2+0]  = wvtx[i*comps+6]; hpUv[i*2+1]  = wvtx[i*comps+7];
            }
            const int sm = prim.material;
            if (sm >= 0 && sm < (int)model.materials.size()) {
                const auto& M = model.materials[sm];
                srcNormalPng = textureBytes(model, M.normalTexture.index);
                srcColorPng  = textureBytes(model, M.pbrMetallicRoughness.baseColorTexture.index);
                srcMrPng     = textureBytes(model, M.pbrMetallicRoughness.metallicRoughnessTexture.index);
                srcAoPng     = textureBytes(model, M.occlusionTexture.index);
                srcEmiPng    = textureBytes(model, M.emissiveTexture.index);
            }
        }

        // Un vertex buffer + index buffer compactes par niveau de LOD.
        // Apres decimation, on ne garde que les vertices reellement references
        // par les indices simplifies -> le vertex count suit le triangle count.
        for (size_t L = 0; L < numLods; ++L) {
            const LodLevel& lvl = cfg.levels[L];

            // Poids des attributs specifiques a CE niveau de LOD.
            // uvWeight augmente avec l'agressivite : force le QEM a rejeter les
            // effondrements d'aretes qui etirent les UV, meme au prix de moins
            // de reduction geometrique.
            std::vector<float> attrW;
            if (hasN)  { attrW.insert(attrW.end(), 3, cfg.normalWeight); }
            if (hasUV) { attrW.insert(attrW.end(), 2, lvl.uvWeight); }
            const size_t attrCount = attrW.size();
            const float* attrPtr = (attrCount > 0) ? (wvtx.data() + 3) : nullptr;

            size_t target = static_cast<size_t>(widx.size() * lvl.ratio);
            target -= target % 3;
            if (target < 3) target = 3;

            // === PROXY LOD : re-depliage atlas + bake de toutes les textures ===
            // Pour les niveaux tres agressifs (ratio < proxyBelow). Decouple le
            // nombre de triangles de l'integrite UV : on re-deplie proprement et
            // on bake albedo + normal + MR + AO + emissive dans un atlas neuf.
            if (cfg.proxy && canBake && lvl.ratio < cfg.proxyBelow) {
                // 1. Soudure par POSITION SEULE : connecte le maillage a travers les
                // coutures UV/normales pour permettre une decimation tres profonde.
                std::vector<unsigned int> premap(vcount);
                const size_t pcount = meshopt_generateVertexRemap(
                    premap.data(), idx.data(), idx.size(), pos.data(), vcount, 3*sizeof(float));
                std::vector<float> ppos(pcount*3);
                std::vector<unsigned int> pidx(idx.size());
                meshopt_remapVertexBuffer(ppos.data(), pos.data(), vcount, 3*sizeof(float), premap.data());
                meshopt_remapIndexBuffer(pidx.data(), idx.data(), idx.size(), premap.data());

                // 2. Decimation geometrique agressive jusqu'a la cible
                std::vector<unsigned int> dlod(pidx.size());
                float derr = 0.0f;
                size_t dn = meshopt_simplify(dlod.data(), pidx.data(), pidx.size(),
                                             ppos.data(), pcount, 3*sizeof(float),
                                             target, 1.0f, 0, &derr);
                dlod.resize(dn);
                std::vector<unsigned int> cr(pcount, ~0u); unsigned int dv = 0;
                for (size_t k = 0; k < dn; ++k) if (cr[dlod[k]] == ~0u) cr[dlod[k]] = dv++;
                std::vector<float> dpos(dv*3);
                for (size_t vi = 0; vi < pcount; ++vi) if (cr[vi] != ~0u) {
                    dpos[cr[vi]*3+0]=ppos[vi*3+0]; dpos[cr[vi]*3+1]=ppos[vi*3+1]; dpos[cr[vi]*3+2]=ppos[vi*3+2];
                }
                std::vector<unsigned int> didx(dn);
                for (size_t k = 0; k < dn; ++k) didx[k] = cr[dlod[k]];
                std::vector<float> dnrm = computeSmoothNormals(dpos, didx);

                // 3. Re-depliage UV propre
                Unwrapped um;
                if (!xatlasUnwrap(dpos, dnrm, didx, cfg.bakeRes, um)) {
                    std::cout << "    LOD" << (L+1) << "  [proxy] echec xatlas, niveau ignore\n";
                    continue;
                }

                // 3. Bake de tous les canaux du materiau source
                struct Ch { const std::vector<unsigned char>* bytes; MapKind kind; int slot; };
                std::vector<Ch> chans = { { &srcNormalPng, MapKind::NormalTangent, 0 } };
                if (!srcColorPng.empty()) chans.push_back({ &srcColorPng, MapKind::Color, 1 });
                if (!srcMrPng.empty())    chans.push_back({ &srcMrPng,    MapKind::Color, 2 });
                if (!srcAoPng.empty())    chans.push_back({ &srcAoPng,    MapKind::Color, 3 });
                if (!srcEmiPng.empty())   chans.push_back({ &srcEmiPng,   MapKind::Color, 4 });
                std::vector<SrcMap> srcs;
                for (auto& ch : chans)
                    srcs.push_back({ ch.bytes->empty() ? nullptr : ch.bytes->data(), ch.bytes->size(), ch.kind });

                BakeHigh high{ hpPos.data(), hpNrm.data(), hpUv.data(), uniqueCount, widx.data(), widx.size() };
                BakeLow  low { um.pos.data(), um.nrm.data(), um.uv.data(), um.vcount, um.idx.data(), um.idx.size() };
                BakeMapsResult br;
                if (!bakeMaps(high, low, srcs.data(), (int)srcs.size(), cfg.bakeRes, cfg.bakeCage, br)) {
                    std::cout << "    LOD" << (L+1) << "  [proxy] echec bake, niveau ignore\n";
                    continue;
                }

                // 4. Emission geometrie (pos/nrm/uv/tangent + indices)
                auto appendVec3 = [&](const std::vector<float>& v, bool minmax)->int {
                    const size_t off = appendData(model, v.data(), v.size()*sizeof(float), 4);
                    const int bv = makeBufferView(model, off, v.size()*sizeof(float), 0, TINYGLTF_TARGET_ARRAY_BUFFER);
                    std::vector<double> mn={1e30,1e30,1e30}, mx={-1e30,-1e30,-1e30};
                    if (minmax) for (size_t i=0;i<v.size()/3;++i) for(int c=0;c<3;++c){ double d=v[i*3+c]; mn[c]=std::min(mn[c],d); mx[c]=std::max(mx[c],d); }
                    return makeAccessor(model, bv, 0, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3,
                                        v.size()/3, minmax?&mn:nullptr, minmax?&mx:nullptr);
                };
                const int aPos = appendVec3(um.pos, true);
                const int aNrm = appendVec3(um.nrm, false);
                const size_t uvOff = appendData(model, um.uv.data(), um.uv.size()*sizeof(float), 4);
                const int uvBv = makeBufferView(model, uvOff, um.uv.size()*sizeof(float), 0, TINYGLTF_TARGET_ARRAY_BUFFER);
                const int aUv  = makeAccessor(model, uvBv, 0, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC2, um.vcount);
                const size_t tOff = appendData(model, br.tangents.data(), br.tangents.size()*sizeof(float), 4);
                const int tBv = makeBufferView(model, tOff, br.tangents.size()*sizeof(float), 0, TINYGLTF_TARGET_ARRAY_BUFFER);
                const int aTan = makeAccessor(model, tBv, 0, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC4, um.vcount);
                const size_t pOff = appendData(model, um.idx.data(), um.idx.size()*sizeof(unsigned int), 4);
                const int iBv = makeBufferView(model, pOff, um.idx.size()*sizeof(unsigned int), 0, TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
                const int aIdx = makeAccessor(model, iBv, 0, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, TINYGLTF_TYPE_SCALAR, um.idx.size());

                // 5. Materiau atlase (clone -> garde les facteurs, remplace les textures)
                tinygltf::Material pm;
                if (prim.material>=0 && prim.material<(int)model.materials.size()) pm = model.materials[prim.material];
                pm.name = (pm.name.empty()?"proxy":pm.name) + "_LOD"+std::to_string(L+1)+"_atlas";
                for (size_t s = 0; s < chans.size(); ++s) {
                    const int tex = addPngTexture(model, br.maps[s].png);
                    switch (chans[s].slot) {
                        case 0: pm.normalTexture.index=tex; pm.normalTexture.texCoord=0; pm.normalTexture.scale=1.0; break;
                        case 1: pm.pbrMetallicRoughness.baseColorTexture.index=tex; pm.pbrMetallicRoughness.baseColorTexture.texCoord=0; break;
                        case 2: pm.pbrMetallicRoughness.metallicRoughnessTexture.index=tex; pm.pbrMetallicRoughness.metallicRoughnessTexture.texCoord=0; break;
                        case 3: pm.occlusionTexture.index=tex; pm.occlusionTexture.texCoord=0; break;
                        case 4: pm.emissiveTexture.index=tex; pm.emissiveTexture.texCoord=0; break;
                    }
                }
                model.materials.push_back(pm);

                Primitive lp;
                lp.mode = TINYGLTF_MODE_TRIANGLES;
                lp.material = (int)model.materials.size()-1;
                lp.indices  = aIdx;
                lp.attributes["POSITION"]   = aPos;
                lp.attributes["NORMAL"]     = aNrm;
                lp.attributes["TEXCOORD_0"] = aUv;
                lp.attributes["TANGENT"]    = aTan;
                lodPrims[L].push_back(lp);

                const int hitPct = br.texelsTotal ? (int)(100.0*double(br.texelsHit)/double(br.texelsTotal)) : 0;
                std::cout << "    LOD" << (L+1) << "  [PROXY] tris " << (widx.size()/3) << " -> " << (um.idx.size()/3)
                          << "  verts " << um.vcount << "  atlas " << cfg.bakeRes << "px x" << (int)srcs.size()
                          << " maps  " << hitPct << "% hits\n";
                continue;
            }

            // 1. Decimation UV-aware via QEM
            // Extraction des UV dans un buffer separe (pas interleave) pour que
            // meshopt_simplifyWithAttributes les lise correctement avec stride=2*4.
            std::vector<float> uvSeparate;
            const float* finalAttrPtr = attrPtr;
            size_t finalAttrStride    = vstride;
            std::vector<float> finalAttrW = attrW;
            if (hasUV && attrCount > 0) {
                const int uvOff = hasN ? 6 : 3;
                uvSeparate.resize(uniqueCount * 2);
                for (size_t vi = 0; vi < uniqueCount; ++vi) {
                    uvSeparate[vi * 2 + 0] = wvtx[vi * comps + uvOff + 0];
                    uvSeparate[vi * 2 + 1] = wvtx[vi * comps + uvOff + 1];
                }
                // Poids : uniquement les 2 composantes UV (plus les normales si presents)
                finalAttrW.clear();
                if (hasN) finalAttrW.insert(finalAttrW.end(), 3, cfg.normalWeight);
                finalAttrW.insert(finalAttrW.end(), 2, lvl.uvWeight);
                // Buffer : normales (interleaved) || UVs (separe) selon presence
                // Cas le plus simple et garanti correct : UVs seules dans buffer non-interleave
                if (!hasN) {
                    finalAttrPtr   = uvSeparate.data();
                    finalAttrStride = 2 * sizeof(float);
                    finalAttrW = { lvl.uvWeight, lvl.uvWeight };
                }
                // Si normales presentes : on garde l'interleave pour les normales
                // mais on passe les UVs separement via une concatenation
                // -> simplification : on ignore les normales dans les attributs et
                //    on passe uniquement les UV dans un buffer compact
                finalAttrPtr    = uvSeparate.data();
                finalAttrStride = 2 * sizeof(float);
                finalAttrW      = { lvl.uvWeight, lvl.uvWeight };
            }
            const size_t finalAttrCount = finalAttrW.size();

            std::vector<unsigned int> lod(widx.size());
            float resultErr = 0.0f;
            const unsigned int opts = cfg.lockBorder ? meshopt_SimplifyLockBorder : 0u;
            size_t n;
            if (finalAttrCount > 0) {
                n = meshopt_simplifyWithAttributes(
                    lod.data(), widx.data(), widx.size(),
                    wvtx.data(), uniqueCount, vstride,
                    finalAttrPtr, finalAttrStride,
                    finalAttrW.data(), finalAttrCount,
                    nullptr, target, lvl.error, opts, &resultErr);
            } else {
                n = meshopt_simplify(
                    lod.data(), widx.data(), widx.size(),
                    wvtx.data(), uniqueCount, vstride,
                    target, lvl.error, opts, &resultErr);
            }

            // Fallback "sloppy" UNIQUEMENT si --sloppy : il fusionne des sommets a
            // travers les coutures UV -> casse les textures. Sinon on accepte plus
            // de triangles que la cible (le QEM preserve la topologie/les UV).
            if (cfg.allowSloppy && n > target * 6 / 5) {
                std::vector<unsigned int> sl(widx.size());
                float se = 0.0f;
                const size_t ns = meshopt_simplifySloppy(
                    sl.data(), widx.data(), widx.size(),
                    wvtx.data(), uniqueCount, vstride,
                    target, lvl.error * 3.0f, &se);
                if (ns >= 3 && ns < n) { lod.swap(sl); n = ns; resultErr = se; }
            }
            lod.resize(n);

            // 2. Compaction : ne garder que les vertices references par lod[].
            // Sans ca, le vertex buffer contient encore tous les uniqueCount vertices
            // du LOD0 alors que LOD3 n'en utilise qu'une fraction.
            std::vector<unsigned int> cremap(uniqueCount, ~0u);
            unsigned int newVCount = 0;
            for (size_t k = 0; k < n; ++k)
                if (cremap[lod[k]] == ~0u) cremap[lod[k]] = newVCount++;

            std::vector<float> cvtx(static_cast<size_t>(newVCount) * comps);
            for (size_t vi = 0; vi < uniqueCount; ++vi)
                if (cremap[vi] != ~0u)
                    std::memcpy(cvtx.data() + cremap[vi] * comps,
                                wvtx.data() + vi * comps, comps * sizeof(float));

            std::vector<unsigned int> cidx(n);
            for (size_t k = 0; k < n; ++k) cidx[k] = cremap[lod[k]];

            // 3. Optimisation du cache GPU sur la representation compacte
            meshopt_optimizeVertexCache(cidx.data(), cidx.data(), n, newVCount);

            // 4. Vertex buffer compacte pour ce LOD
            const size_t vOff = appendData(model, cvtx.data(), cvtx.size() * sizeof(float), 4);
            const int vView = makeBufferView(model, vOff, cvtx.size() * sizeof(float),
                                             vstride, TINYGLTF_TARGET_ARRAY_BUFFER);
            std::vector<double> mn = { 1e30, 1e30, 1e30 }, mx = { -1e30, -1e30, -1e30 };
            for (size_t i = 0; i < newVCount; ++i)
                for (int c = 0; c < 3; ++c) {
                    const double v = cvtx[i * comps + c];
                    mn[c] = std::min(mn[c], v); mx[c] = std::max(mx[c], v);
                }
            const int posOut = makeAccessor(model, vView, 0, TINYGLTF_COMPONENT_TYPE_FLOAT,
                                            TINYGLTF_TYPE_VEC3, newVCount, &mn, &mx);
            const int nrmOut = hasN  ? makeAccessor(model, vView, 3*sizeof(float),
                                        TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, newVCount) : -1;
            const int uvOut  = hasUV ? makeAccessor(model, vView, (hasN?6:3)*sizeof(float),
                                        TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC2, newVCount) : -1;

            // 5. Index buffer
            const size_t iOff = appendData(model, cidx.data(), n * sizeof(unsigned int), 4);
            const int iView = makeBufferView(model, iOff, n * sizeof(unsigned int), 0,
                                             TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
            const int iAcc = makeAccessor(model, iView, 0, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                                          TINYGLTF_TYPE_SCALAR, n);

            Primitive lp;
            lp.mode = TINYGLTF_MODE_TRIANGLES;
            lp.material = prim.material;
            lp.indices  = iAcc;
            lp.attributes["POSITION"] = posOut;
            if (nrmOut >= 0) lp.attributes["NORMAL"]     = nrmOut;
            if (uvOut  >= 0) lp.attributes["TEXCOORD_0"] = uvOut;

            // 6. Bake de normal map (re-projection du detail haute-poly).
            std::string bakeInfo;
            if (canBake && lvl.ratio <= cfg.bakeMinRatio) {
                // Deinterleave low-poly
                std::vector<float> lpPos(newVCount*3), lpNrm(newVCount*3), lpUv(newVCount*2);
                const int uvOff = hasN ? 6 : 3;
                for (size_t i = 0; i < newVCount; ++i) {
                    lpPos[i*3+0]=cvtx[i*comps+0]; lpPos[i*3+1]=cvtx[i*comps+1]; lpPos[i*3+2]=cvtx[i*comps+2];
                    lpNrm[i*3+0]=cvtx[i*comps+3]; lpNrm[i*3+1]=cvtx[i*comps+4]; lpNrm[i*3+2]=cvtx[i*comps+5];
                    lpUv[i*2+0]=cvtx[i*comps+uvOff+0]; lpUv[i*2+1]=cvtx[i*comps+uvOff+1];
                }
                BakeHigh high{ hpPos.data(), hpNrm.data(), hpUv.data(), uniqueCount, widx.data(), widx.size() };
                BakeLow  low { lpPos.data(), lpNrm.data(), lpUv.data(), newVCount, cidx.data(), cidx.size() };
                BakeResult br;
                const unsigned char* srcPtr = srcNormalPng.empty() ? nullptr : srcNormalPng.data();
                if (bakeNormalMap(high, low, srcPtr, srcNormalPng.size(), cfg.bakeRes, cfg.bakeCage, br)) {
                    // TANGENT (necessaire pour lire la map dans la bonne base)
                    const size_t tOff = appendData(model, br.tangents.data(),
                                                    br.tangents.size()*sizeof(float), 4);
                    const int tView = makeBufferView(model, tOff, br.tangents.size()*sizeof(float),
                                                     4*sizeof(float), TINYGLTF_TARGET_ARRAY_BUFFER);
                    const int tAcc = makeAccessor(model, tView, 0, TINYGLTF_COMPONENT_TYPE_FLOAT,
                                                  TINYGLTF_TYPE_VEC4, newVCount);
                    lp.attributes["TANGENT"] = tAcc;
                    // Texture + materiau dedie
                    const int texIdx = addPngTexture(model, br.png);
                    lp.material = cloneMaterialWithNormal(model, prim.material, texIdx,
                                  "_LOD" + std::to_string(L+1) + "_nrm");
                    const int hitPct = br.texelsTotal
                        ? (int)(100.0 * double(br.texelsHit) / double(br.texelsTotal)) : 0;
                    bakeInfo = "  [bake " + std::to_string(br.width) + "px, "
                               + std::to_string(hitPct) + "% hits"
                               + (br.usedSourceMap ? ", +detail map" : ", geo only") + "]";
                }
            }
            lodPrims[L].push_back(lp);

            const bool overTarget = n > target * 11 / 10;
            std::cout << "    LOD" << (L + 1)
                      << "  tris " << (widx.size() / 3) << " -> " << (n / 3)
                      << "  verts " << uniqueCount << " -> " << newVCount
                      << "  (" << int(100.0 * double(n) / double(widx.size())) << "%)"
                      << "  err~" << (resultErr * scale)
                      << (overTarget ? "  [+tris preserves UV]" : "")
                      << bakeInfo
                      << "\n";
        }
    }

    // Materialise un mesh par niveau de LOD
    std::vector<int> out(numLods, -1);
    for (size_t L = 0; L < numLods; ++L) {
        if (lodPrims[L].empty()) continue;
        Mesh m;
        m.name = (src.name.empty() ? ("mesh" + std::to_string(meshIndex)) : src.name)
                 + "_LOD" + std::to_string(L + 1);
        m.primitives = std::move(lodPrims[L]);
        model.meshes.push_back(std::move(m));
        out[L] = static_cast<int>(model.meshes.size() - 1);
    }
    return out;
}

static bool meshIsSkinned(const Model& m, int meshIndex) {
    for (const auto& p : m.meshes[meshIndex].primitives)
        if (p.attributes.count("JOINTS_0")) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Traitement complet d'un modele
// ---------------------------------------------------------------------------
static int processModel(Model& model, const Config& cfg) {
    if (model.buffers.empty()) { model.buffers.emplace_back(); }

    const size_t nodeCount = model.nodes.size();
    std::vector<std::vector<int>> meshLodCache(model.meshes.size()); // meshIndex -> lod mesh idx
    std::vector<char> cached(model.meshes.size(), 0);
    int processed = 0;

    for (size_t i = 0; i < nodeCount; ++i) {
        const int meshIndex = model.nodes[i].mesh;
        if (meshIndex < 0) continue;
        if (model.nodes[i].skin >= 0 || meshIsSkinned(model, meshIndex)) {
            std::cout << "  node " << i << " : mesh skinne, ignore.\n";
            continue;
        }

        std::cout << "  node " << i << " -> mesh " << meshIndex
                  << " (" << model.meshes[meshIndex].name << ")\n";

        if (!cached[meshIndex]) {
            meshLodCache[meshIndex] = generateLodMeshes(model, meshIndex, cfg);
            cached[meshIndex] = 1;
        }
        const std::vector<int>& lodMeshes = meshLodCache[meshIndex];

        // Cree un node par niveau de LOD, reference via MSFT_lod.ids
        tinygltf::Value::Array ids;
        for (int lm : lodMeshes) {
            if (lm < 0) continue;
            Node n;
            n.name = model.meshes[lm].name;
            n.mesh = lm;
            model.nodes.push_back(n);
            ids.push_back(tinygltf::Value(int(model.nodes.size() - 1)));
        }
        if (ids.empty()) continue;

        // MSFT_lod sur le node LOD0 (qui garde son mesh d'origine intact)
        tinygltf::Value::Object lodExt;
        lodExt["ids"] = tinygltf::Value(ids);
        model.nodes[i].extensions["MSFT_lod"] = tinygltf::Value(lodExt);

        // CRITIQUE : rend les nodes LOD accessibles depuis la scene.
        // Spec MSFT_lod : les nodes reference dans "ids" doivent etre des freres
        // du node LOD0 dans la hierarchie. Les importeurs (dont UnityGLTF) ignorent
        // les nodes orphelins (non atteignables depuis la scene).
        //
        // Cas 1 : node i a un parent -> LOD1/2/3 deviennent children du meme parent.
        // Cas 2 : node i est racine de scene -> LOD1/2/3 vont aussi a la racine.
        bool addedToParent = false;
        for (size_t j = 0; j < nodeCount && !addedToParent; ++j) {
            const auto& ch = model.nodes[j].children;
            if (std::find(ch.begin(), ch.end(), (int)i) != ch.end()) {
                for (const auto& id : ids)
                    model.nodes[j].children.push_back(id.Get<int>());
                addedToParent = true;
            }
        }
        if (!addedToParent && !model.scenes.empty()) {
            const int sceneIdx = (model.defaultScene >= 0) ? model.defaultScene : 0;
            for (const auto& id : ids)
                model.scenes[sceneIdx].nodes.push_back(id.Get<int>());
        }

        // MSFT_screencoverage : un seuil par niveau (LOD0..LODn)
        tinygltf::Value::Array cov;
        cov.push_back(tinygltf::Value(double(1.0)));
        for (const auto& lvl : cfg.levels) cov.push_back(tinygltf::Value(double(lvl.coverage)));
        tinygltf::Value::Object extras;
        extras["MSFT_screencoverage"] = tinygltf::Value(cov);
        model.nodes[i].extras = tinygltf::Value(extras);

        ++processed;
    }

    if (processed > 0) {
        if (std::find(model.extensionsUsed.begin(), model.extensionsUsed.end(), "MSFT_lod")
            == model.extensionsUsed.end())
            model.extensionsUsed.push_back("MSFT_lod");
    }

    // Buffer auto-contenu a l'ecriture
    model.buffers[0].uri.clear();
    return processed;
}

// ---------------------------------------------------------------------------
// Generateur d'asset de test (sphere UV densifiee) pour valider sans asset externe
// ---------------------------------------------------------------------------
static bool generateTestSphere(const std::string& outPath, int rings = 64, int sectors = 64) {
    Model model;
    model.asset.version = "2.0";
    model.asset.generator = "AutoLOD test";
    model.buffers.emplace_back();

    std::vector<float> pos, nrm, uv;
    for (int r = 0; r <= rings; ++r) {
        const float v = float(r) / rings;
        const float phi = v * 3.14159265358979f;
        for (int s = 0; s <= sectors; ++s) {
            const float u = float(s) / sectors;
            const float theta = u * 2.0f * 3.14159265358979f;
            const float x = std::sin(phi) * std::cos(theta);
            const float y = std::cos(phi);
            const float z = std::sin(phi) * std::sin(theta);
            pos.insert(pos.end(), { x, y, z });
            nrm.insert(nrm.end(), { x, y, z });
            uv.insert(uv.end(), { u, v });
        }
    }
    std::vector<uint32_t> idx;
    const int stride = sectors + 1;
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < sectors; ++s) {
            const uint32_t a = r * stride + s, b = a + stride;
            idx.insert(idx.end(), { a, b, a + 1, a + 1, b, b + 1 });
        }

    const size_t vcount = pos.size() / 3;
    const size_t pOff = appendData(model, pos.data(), pos.size() * 4);
    const size_t nOff = appendData(model, nrm.data(), nrm.size() * 4);
    const size_t tOff = appendData(model, uv.data(),  uv.size()  * 4);
    const size_t iOff = appendData(model, idx.data(), idx.size() * 4);

    const int pv = makeBufferView(model, pOff, pos.size() * 4, 0, TINYGLTF_TARGET_ARRAY_BUFFER);
    const int nv = makeBufferView(model, nOff, nrm.size() * 4, 0, TINYGLTF_TARGET_ARRAY_BUFFER);
    const int tv = makeBufferView(model, tOff, uv.size()  * 4, 0, TINYGLTF_TARGET_ARRAY_BUFFER);
    const int iv = makeBufferView(model, iOff, idx.size() * 4, 0, TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);

    std::vector<double> mn = { -1, -1, -1 }, mx = { 1, 1, 1 };
    const int pa = makeAccessor(model, pv, 0, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, vcount, &mn, &mx);
    const int na = makeAccessor(model, nv, 0, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, vcount);
    const int ta = makeAccessor(model, tv, 0, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC2, vcount);
    const int ia = makeAccessor(model, iv, 0, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, TINYGLTF_TYPE_SCALAR, idx.size());

    tinygltf::Material mat; mat.name = "TestMat";
    mat.pbrMetallicRoughness.baseColorFactor = { 0.8, 0.5, 0.3, 1.0 };
    model.materials.push_back(mat);

    Primitive prim; prim.mode = TINYGLTF_MODE_TRIANGLES; prim.material = 0; prim.indices = ia;
    prim.attributes["POSITION"] = pa; prim.attributes["NORMAL"] = na; prim.attributes["TEXCOORD_0"] = ta;
    Mesh mesh; mesh.name = "TestSphere"; mesh.primitives.push_back(prim);
    model.meshes.push_back(mesh);

    Node node; node.name = "TestSphere"; node.mesh = 0;
    model.nodes.push_back(node);
    model.scenes.emplace_back(); model.scenes[0].nodes.push_back(0); model.defaultScene = 0;

    tinygltf::TinyGLTF ctx;
    const bool bin = outPath.size() > 4 && outPath.substr(outPath.size() - 4) == ".glb";
    return ctx.WriteGltfSceneToFile(&model, outPath, true, true, true, bin);
}

// ---------------------------------------------------------------------------
// Extrait UN seul mesh (via son node) dans un Model minimal et autonome :
// ne conserve que les accessors / bufferViews / octets / materiaux / textures /
// images reellement utilises, avec un buffer neuf compacte. Indispensable pour
// que chaque fichier split ne pese que ce qu'il contient vraiment.
// ---------------------------------------------------------------------------
static Model extractSingleMesh(const Model& src, int nodeIdx) {
    Model out;
    out.asset = src.asset;
    out.buffers.emplace_back(); // buffer 0 neuf

    const Mesh& srcMesh = src.meshes[src.nodes[nodeIdx].mesh];

    // 1. Collecte materiaux + accessors utilises par les primitives
    std::vector<int> usedMat;
    std::vector<int> usedAcc;
    auto addAcc = [&](int a){ if (a >= 0) usedAcc.push_back(a); };
    for (const auto& p : srcMesh.primitives) {
        if (p.material >= 0) usedMat.push_back(p.material);
        for (const auto& [k, a] : p.attributes) addAcc(a);
        addAcc(p.indices);
    }

    // 2. Materiaux -> textures -> images + samplers
    auto matTextures = [](const tinygltf::Material& m){
        std::vector<int> t;
        t.push_back(m.pbrMetallicRoughness.baseColorTexture.index);
        t.push_back(m.pbrMetallicRoughness.metallicRoughnessTexture.index);
        t.push_back(m.normalTexture.index);
        t.push_back(m.occlusionTexture.index);
        t.push_back(m.emissiveTexture.index);
        return t;
    };
    std::vector<int> usedTex, usedImg, usedSamp;
    for (int mi : usedMat)
        for (int ti : matTextures(src.materials[mi]))
            if (ti >= 0) usedTex.push_back(ti);
    for (int ti : usedTex) {
        if (src.textures[ti].source  >= 0) usedImg.push_back(src.textures[ti].source);
        if (src.textures[ti].sampler >= 0) usedSamp.push_back(src.textures[ti].sampler);
    }

    // 3. BufferViews utilises = ceux des accessors + ceux des images
    std::vector<int> usedBV;
    for (int a : usedAcc) if (src.accessors[a].bufferView >= 0) usedBV.push_back(src.accessors[a].bufferView);
    for (int im : usedImg) if (src.images[im].bufferView   >= 0) usedBV.push_back(src.images[im].bufferView);
    std::sort(usedBV.begin(), usedBV.end());
    usedBV.erase(std::unique(usedBV.begin(), usedBV.end()), usedBV.end());

    // 4. Buffer neuf : recopie chaque bufferView, remap des offsets
    std::map<int,int> bvMap;
    for (int bv : usedBV) {
        const BufferView& s = src.bufferViews[bv];
        const Buffer& sb = src.buffers[s.buffer];
        const size_t off = appendData(out, sb.data.data() + s.byteOffset, s.byteLength, 4);
        BufferView nb;
        nb.buffer = 0; nb.byteOffset = off; nb.byteLength = s.byteLength;
        nb.byteStride = s.byteStride; nb.target = s.target;
        out.bufferViews.push_back(nb);
        bvMap[bv] = (int)out.bufferViews.size() - 1;
    }

    // Helper : dedup + remap generique
    auto remap = [](std::vector<int>& used){
        std::sort(used.begin(), used.end());
        used.erase(std::unique(used.begin(), used.end()), used.end());
        std::map<int,int> m;
        for (size_t i = 0; i < used.size(); ++i) m[used[i]] = (int)i;
        return m;
    };

    // 5. Accessors
    std::map<int,int> accMap = remap(usedAcc);
    out.accessors.resize(usedAcc.size());
    for (auto [oldA, newA] : accMap) {
        Accessor a = src.accessors[oldA];
        a.bufferView = bvMap[a.bufferView];
        out.accessors[newA] = a;
    }

    // 6. Images / samplers / textures / materiaux
    std::map<int,int> imgMap  = remap(usedImg);
    std::map<int,int> sampMap = remap(usedSamp);
    std::map<int,int> texMap  = remap(usedTex);
    std::map<int,int> matMap  = remap(usedMat);

    out.images.resize(imgMap.size());
    for (auto [o, nw] : imgMap) {
        tinygltf::Image im = src.images[o];
        if (im.bufferView >= 0) im.bufferView = bvMap[im.bufferView];
        out.images[nw] = im;
    }
    out.samplers.resize(sampMap.size());
    for (auto [o, nw] : sampMap) out.samplers[nw] = src.samplers[o];
    out.textures.resize(texMap.size());
    for (auto [o, nw] : texMap) {
        tinygltf::Texture t = src.textures[o];
        if (t.source  >= 0) t.source  = imgMap[t.source];
        if (t.sampler >= 0) t.sampler = sampMap[t.sampler];
        out.textures[nw] = t;
    }
    out.materials.resize(matMap.size());
    for (auto [o, nw] : matMap) {
        tinygltf::Material m = src.materials[o];
        auto fix = [&](int& idx){ if (idx >= 0) idx = texMap[idx]; };
        fix(m.pbrMetallicRoughness.baseColorTexture.index);
        fix(m.pbrMetallicRoughness.metallicRoughnessTexture.index);
        fix(m.normalTexture.index);
        fix(m.occlusionTexture.index);
        fix(m.emissiveTexture.index);
        out.materials[nw] = m;
    }

    // 7. Mesh + primitives remappes
    Mesh m;
    m.name = srcMesh.name;
    for (const auto& sp : srcMesh.primitives) {
        Primitive p;
        p.mode = sp.mode;
        p.material = (sp.material >= 0) ? matMap[sp.material] : -1;
        p.indices  = (sp.indices  >= 0) ? accMap[sp.indices]  : -1;
        for (const auto& [k, a] : sp.attributes) p.attributes[k] = accMap[a];
        m.primitives.push_back(p);
    }
    out.meshes.push_back(m);

    // 8. Node + scene (transform du node source conserve)
    Node n = src.nodes[nodeIdx];
    n.mesh = 0;
    n.children.clear();
    n.extensions.erase("MSFT_lod");
    n.skin = -1;
    out.nodes.push_back(n);
    out.scenes.emplace_back();
    out.scenes[0].nodes.push_back(0);
    out.defaultScene = 0;
    return out;
}

// ---------------------------------------------------------------------------
// Mode --split : ecrit un GLB minimal et autonome par niveau de LOD.
// Pratique pour creer un LODGroup manuel dans Unity (moteurs sans MSFT_lod).
// ---------------------------------------------------------------------------
static void writeSplitFiles(const Model& model, const std::string& outputBase,
                            tinygltf::TinyGLTF& ctx) {
    const fs::path base(outputBase);
    const std::string dir  = base.parent_path().string();
    std::string       stem = base.stem().string();
    if (stem.size() > 4 && stem.substr(stem.size() - 4) == "_lod")
        stem = stem.substr(0, stem.size() - 4);

    // Collecte les groupes LOD (nodes portant MSFT_lod)
    struct LodGroup { int lod0; std::vector<int> lods; };
    std::vector<LodGroup> groups;
    for (int ni = 0; ni < (int)model.nodes.size(); ++ni) {
        if (!model.nodes[ni].extensions.count("MSFT_lod")) continue;
        LodGroup g; g.lod0 = ni;
        const auto& arr = model.nodes[ni].extensions.at("MSFT_lod")
            .Get<tinygltf::Value::Object>().at("ids").Get<tinygltf::Value::Array>();
        for (const auto& v : arr) g.lods.push_back(v.Get<int>());
        groups.push_back(g);
    }
    if (groups.empty()) { std::cerr << "[warn] aucun groupe LOD pour --split\n"; return; }

    for (const LodGroup& g : groups) {
        std::vector<int> all = { g.lod0 };
        all.insert(all.end(), g.lods.begin(), g.lods.end());

        for (int L = 0; L < (int)all.size(); ++L) {
            if (model.nodes[all[L]].mesh < 0) continue;
            Model one = extractSingleMesh(model, all[L]);
            const std::string path = (fs::path(dir) / (stem + "_LOD" + std::to_string(L) + ".glb")).string();
            if (ctx.WriteGltfSceneToFile(&one, path, true, true, true, true))
                std::cout << "Split ecrit : " << path << "\n";
            else
                std::cerr << "[err] split echoue : " << path << "\n";
        }
    }
}

// ---------------------------------------------------------------------------
static void usage() {
    std::cout <<
        "AutoLOD - generateur de LOD pour meshs statiques glTF/GLB\n\n"
        "Usage : autolod <input.gltf|.glb> [output.glb] [options]\n"
        "        autolod --gen-test <sphere.glb>\n\n"
        "Options :\n"
        "  --ratios a,b,c     ratios d'index par LOD     (defaut 0.6,0.3,0.1)\n"
        "  --errors a,b,c     erreurs relatives par LOD   (defaut 0.01,0.02,0.05)\n"
        "  --lock-border      verrouille les bords (assets modulaires)\n"
        "  --sloppy           autorise la decimation sloppy (CASSE les UV/textures,\n"
        "                     reserve aux LOD tres lointains sans dependance texture)\n"
        "  --uv-weight f      meme poids UV pour tous les LOD  (ex: 4.0)\n"
        "  --uv-weights a,b,c poids UV par niveau             (defaut 1.0,3.0,10.0)\n"
        "                     valeur haute = moins de distorsion de texture\n"
        "                     augmente si LOD3 deforme la texture\n"
        "  --normal-weight f  poids QEM des normales          (defaut 0.5)\n"
        "  --bake             bake une normal map haute->basse poly par LOD\n"
        "                     (restaure le detail de surface : LA solution qualite)\n"
        "  --bake-res N       resolution de la normal map      (defaut 1024)\n"
        "  --bake-cage f      distance de projection, frac. bbox (defaut 0.03)\n"
        "  --proxy            LOD tres lointains (ratio < 0.15) : re-depliage UV\n"
        "                     (xatlas) + bake albedo+normal+MR+AO+emissive en atlas.\n"
        "                     Tres peu de tris mais ressemble aux LOD superieurs.\n"
        "                     Implique --bake.\n"
        "  --split            ecrit un GLB minimal autonome par niveau LOD\n"
        "                     (ex: mesh_LOD0.glb, mesh_LOD1.glb, ...)\n"
        "                     chaque fichier ne contient QUE les donnees de son LOD\n"
        "                     -> LODGroup manuel dans Unity / moteurs sans MSFT_lod\n";
}

static std::vector<float> parseFloats(const std::string& s) {
    std::vector<float> v; size_t p = 0;
    while (p < s.size()) {
        size_t c = s.find(',', p);
        if (c == std::string::npos) c = s.size();
        v.push_back(std::stof(s.substr(p, c - p)));
        p = c + 1;
    }
    return v;
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }

    std::string a1 = argv[1];
    if (a1 == "-h" || a1 == "--help") { usage(); return 0; }
    if (a1 == "--gen-test") {
        if (argc < 3) { std::cerr << "Chemin de sortie manquant.\n"; return 1; }
        if (!generateTestSphere(argv[2])) { std::cerr << "Echec ecriture test.\n"; return 1; }
        std::cout << "Sphere de test ecrite : " << argv[2] << "\n";
        return 0;
    }
    if (a1 == "--dump-images") { // debug : extrait toutes les images d'un GLB
        if (argc < 4) { std::cerr << "Usage: --dump-images <in.glb> <prefix>\n"; return 1; }
        tinygltf::TinyGLTF c;
        c.SetImageLoader([](tinygltf::Image* img,const int,std::string*,std::string*,
                            int,int,const unsigned char* d,int s,void*)->bool{
            img->as_is=true; img->image.assign(d,d+s); return true; }, nullptr);
        Model m; std::string e,w;
        if (!c.LoadBinaryFromFile(&m,&e,&w,argv[2])) { std::cerr<<e<<"\n"; return 1; }
        for (size_t i = 0; i < m.images.size(); ++i) {
            const std::string ext = (m.images[i].mimeType=="image/jpeg")?".jpg":".png";
            const std::string fn = std::string(argv[3]) + std::to_string(i) + ext;
            FILE* f = std::fopen(fn.c_str(), "wb");
            if (f) { std::fwrite(m.images[i].image.data(),1,m.images[i].image.size(),f); std::fclose(f); }
            std::cout << "image " << i << " -> " << fn << " (" << m.images[i].image.size() << " o)\n";
        }
        return 0;
    }

    const std::string input = a1;
    std::string output;
    Config cfg;
    std::vector<float> ratios, errors, uvWeights;
    float singleUv = -1.0f; // --uv-weight (applique a tous), -1 = non fourni
    bool splitMode = false;

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--lock-border")                    cfg.lockBorder = true;
        else if (a == "--sloppy")                         cfg.allowSloppy = true;
        else if (a == "--split")                          splitMode = true;
        else if (a == "--ratios"        && i+1 < argc)   ratios    = parseFloats(argv[++i]);
        else if (a == "--errors"        && i+1 < argc)   errors    = parseFloats(argv[++i]);
        else if (a == "--uv-weights"    && i+1 < argc)   uvWeights = parseFloats(argv[++i]);
        else if (a == "--uv-weight"     && i+1 < argc)   singleUv  = std::stof(argv[++i]);
        else if (a == "--normal-weight" && i+1 < argc)   cfg.normalWeight = std::stof(argv[++i]);
        else if (a == "--bake")                           cfg.bake = true;
        else if (a == "--bake-res"      && i+1 < argc)   cfg.bakeRes = std::stoi(argv[++i]);
        else if (a == "--bake-cage"     && i+1 < argc)   cfg.bakeCage = std::stof(argv[++i]);
        else if (a == "--proxy")                        { cfg.proxy = true; cfg.bake = true; }
        else if (a[0] != '-')                             output = a;
        else { std::cerr << "Option inconnue : " << a << "\n"; return 1; }
    }

    // Niveaux personnalises via --ratios
    if (!ratios.empty()) {
        cfg.levels.clear();
        for (size_t i = 0; i < ratios.size(); ++i) {
            LodLevel l;
            l.ratio       = ratios[i];
            l.error       = (i < errors.size())     ? errors[i]     : 0.02f * float(i + 1);
            l.allowSloppy = false; // sloppy pilote globalement par --sloppy
            l.coverage    = std::max(0.02f, 0.5f / float(i + 1));
            l.uvWeight    = 1.0f * std::pow(3.0f, float(i)); // 1, 3, 9, ... (surcharge plus bas)
            cfg.levels.push_back(l);
        }
    }

    // Poids UV : explicite (--uv-weights / --uv-weight) sinon defaut selon le mode.
    const bool uvUserSet = !uvWeights.empty() || singleUv >= 0.0f;
    if (!uvWeights.empty())
        for (size_t i = 0; i < uvWeights.size() && i < cfg.levels.size(); ++i)
            cfg.levels[i].uvWeight = uvWeights[i];
    if (singleUv >= 0.0f)
        for (auto& lvl : cfg.levels) lvl.uvWeight = singleUv;

    // Avec --bake, le detail est restaure par la normal map : on decime
    // agressivement (poids UV = 1) pour respecter les ratios demandes, sauf si
    // l'utilisateur a impose ses propres poids.
    if (cfg.bake && !uvUserSet)
        for (auto& lvl : cfg.levels) lvl.uvWeight = 1.0f;

    if (output.empty()) {
        fs::path p(input);
        output = (p.parent_path() / (p.stem().string() + "_lod.glb")).string();
    }

    tinygltf::TinyGLTF ctx;
    // Callback no-op : on ne decode pas les pixels, on les garde en pass-through.
    // Sans ca, tinygltf refuse de charger quand TINYGLTF_NO_STB_IMAGE est defini.
    // Loader no-op : stocke les bytes bruts sans decoder les pixels.
    ctx.SetImageLoader(
        [](tinygltf::Image* img, const int, std::string*, std::string*,
           int, int, const unsigned char* data, int size, void*) -> bool {
            img->as_is = true;
            img->image.assign(data, data + size);
            return true;
        }, nullptr);
    // Writer no-op : les images restent dans leurs bufferViews d'origine.
    ctx.SetImageWriter(
        [](const std::string*, const std::string*, const tinygltf::Image*,
           bool, const tinygltf::FsCallbacks*, const tinygltf::URICallbacks*,
           std::string*, void*) -> bool {
            return true;
        }, nullptr);
    Model model;
    std::string err, warn;
    const bool isBinaryIn = input.size() > 4 && input.substr(input.size() - 4) == ".glb";
    const bool ok = isBinaryIn
        ? ctx.LoadBinaryFromFile(&model, &err, &warn, input)
        : ctx.LoadASCIIFromFile(&model, &err, &warn, input);
    if (!warn.empty()) std::cout << "[warn] " << warn << "\n";
    if (!ok) { std::cerr << "[err] chargement echoue : " << err << "\n"; return 1; }

    std::cout << "Chargement OK : " << model.meshes.size() << " mesh(s), "
              << model.nodes.size() << " node(s)\n";

    const int processed = processModel(model, cfg);
    std::cout << "Meshs traites : " << processed << "\n";

    const bool isBinaryOut = output.size() > 4 && output.substr(output.size() - 4) == ".glb";
    if (!ctx.WriteGltfSceneToFile(&model, output, true, true, true, isBinaryOut)) {
        std::cerr << "[err] ecriture echouee : " << output << "\n";
        return 1;
    }
    std::cout << "Ecrit : " << output << "\n";

    if (splitMode) writeSplitFiles(model, output, ctx);

    return 0;
}
