// baker.cpp - implementation du bake de normal map composite (voir baker.h)

#include "baker.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

// --------------------------------------------------------------------------
// Algebre vectorielle
// --------------------------------------------------------------------------
struct V3 {
    float x, y, z;
    V3 operator+(const V3& o) const { return { x + o.x, y + o.y, z + o.z }; }
    V3 operator-(const V3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    V3 operator*(float s)     const { return { x * s, y * s, z * s }; }
};
inline float dot(const V3& a, const V3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline V3 cross(const V3& a, const V3& b) {
    return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
inline V3 norm(const V3& v) {
    const float l = std::sqrt(dot(v, v));
    return (l > 1e-20f) ? V3{ v.x/l, v.y/l, v.z/l } : V3{ 0, 0, 1 };
}
inline V3 getV3(const float* p, size_t i) { return { p[i*3+0], p[i*3+1], p[i*3+2] }; }
inline float fract(float x) { return x - std::floor(x); }

// --------------------------------------------------------------------------
// Image source (normal map) decodee
// --------------------------------------------------------------------------
struct SrcImage {
    int w = 0, h = 0, ch = 0;
    unsigned char* data = nullptr;
    bool valid() const { return data && w > 0 && h > 0; }
    SrcImage() = default;
    ~SrcImage() { if (data) stbi_image_free(data); }
    SrcImage(const SrcImage&) = delete;
    SrcImage& operator=(const SrcImage&) = delete;
    SrcImage(SrcImage&& o) noexcept { *this = std::move(o); }
    SrcImage& operator=(SrcImage&& o) noexcept {
        if (this != &o) { if (data) stbi_image_free(data);
            w=o.w; h=o.h; ch=o.ch; data=o.data; o.data=nullptr; }
        return *this;
    }
    void load(const unsigned char* png, size_t size) {
        if (png && size) data = stbi_load_from_memory(png, (int)size, &w, &h, &ch, 0);
    }

    // Echantillonne en RGB brut [0,1]. UV glTF (origine haut-gauche, REPEAT). Bilineaire.
    V3 sampleColor(float u, float v) const {
        u = fract(u); v = fract(v);
        const float fx = u * w - 0.5f, fy = v * h - 0.5f;
        int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
        const float tx = fx - x0, ty = fy - y0;
        auto wrap = [](int a, int n){ a %= n; return a < 0 ? a + n : a; };
        const int x1 = wrap(x0+1, w), y1 = wrap(y0+1, h);
        x0 = wrap(x0, w); y0 = wrap(y0, h);
        auto texel = [&](int x, int y){
            const unsigned char* p = data + ((size_t)y*w + x)*ch;
            return V3{ p[0]/255.0f, (ch>=2?p[1]:p[0])/255.0f, (ch>=3?p[2]:p[0])/255.0f };
        };
        const V3 a = texel(x0,y0), b = texel(x1,y0), c = texel(x0,y1), d = texel(x1,y1);
        return (a*(1-tx)+b*tx)*(1-ty) + (c*(1-tx)+d*tx)*ty;
    }
    // Idem mais decode en normale tangent-space [-1,1] (et renormalise).
    V3 sampleNormal(float u, float v) const {
        const V3 c = sampleColor(u, v);
        return norm(V3{ c.x*2-1, c.y*2-1, c.z*2-1 });
    }
};

// --------------------------------------------------------------------------
// BVH median-split sur les triangles haute-poly
// --------------------------------------------------------------------------
struct Tri { V3 v0, v1, v2; unsigned int i0, i1, i2; };

struct AABB {
    V3 mn{  1e30f,  1e30f,  1e30f };
    V3 mx{ -1e30f, -1e30f, -1e30f };
    void grow(const V3& p) {
        mn.x=std::min(mn.x,p.x); mn.y=std::min(mn.y,p.y); mn.z=std::min(mn.z,p.z);
        mx.x=std::max(mx.x,p.x); mx.y=std::max(mx.y,p.y); mx.z=std::max(mx.z,p.z);
    }
};

struct BVHNode { AABB box; int start=0,count=0,left=-1,right=-1; };

struct BVH {
    std::vector<Tri> tris;
    std::vector<int> order;
    std::vector<BVHNode> nodes;

    int build(int start, int count) {
        BVHNode node;
        for (int k = 0; k < count; ++k) {
            const Tri& t = tris[order[start+k]];
            node.box.grow(t.v0); node.box.grow(t.v1); node.box.grow(t.v2);
        }
        const int self = (int)nodes.size();
        nodes.push_back(node);
        if (count <= 4) { nodes[self].start = start; nodes[self].count = count; return self; }

        const V3 ext = nodes[self].box.mx - nodes[self].box.mn;
        const int axis = (ext.x > ext.y && ext.x > ext.z) ? 0 : (ext.y > ext.z ? 1 : 2);
        auto centroid = [&](int ti){
            const Tri& t = tris[ti];
            const V3 c = (t.v0 + t.v1 + t.v2) * (1.0f/3.0f);
            return axis==0 ? c.x : (axis==1 ? c.y : c.z);
        };
        std::sort(order.begin()+start, order.begin()+start+count,
                  [&](int a,int b){ return centroid(a) < centroid(b); });
        const int mid = count/2;
        const int l = build(start, mid);
        const int r = build(start+mid, count-mid);
        nodes[self].left = l; nodes[self].right = r; nodes[self].count = 0;
        return self;
    }

    void buildFrom(const BakeHigh& h) {
        tris.reserve(h.icount/3);
        for (size_t i = 0; i+2 < h.icount; i += 3) {
            Tri t; t.i0=h.idx[i]; t.i1=h.idx[i+1]; t.i2=h.idx[i+2];
            t.v0=getV3(h.pos,t.i0); t.v1=getV3(h.pos,t.i1); t.v2=getV3(h.pos,t.i2);
            tris.push_back(t);
        }
        order.resize(tris.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
        nodes.reserve(tris.size()*2);
        if (!tris.empty()) build(0, (int)tris.size());
    }
};

inline bool hitAABB(const AABB& b, const V3& o, const V3& invDir, float tMax) {
    float t0 = 0.0f, t1 = tMax;
    for (int a = 0; a < 3; ++a) {
        const float org = a==0?o.x:(a==1?o.y:o.z);
        const float inv = a==0?invDir.x:(a==1?invDir.y:invDir.z);
        const float mn  = a==0?b.mn.x:(a==1?b.mn.y:b.mn.z);
        const float mx  = a==0?b.mx.x:(a==1?b.mx.y:b.mx.z);
        float ta = (mn-org)*inv, tb = (mx-org)*inv;
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta); t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    return true;
}

inline float rayTri(const V3& o, const V3& d, const Tri& t, float& u, float& v) {
    const V3 e1 = t.v1-t.v0, e2 = t.v2-t.v0, p = cross(d, e2);
    const float det = dot(e1, p);
    if (std::fabs(det) < 1e-12f) return -1.0f;
    const float inv = 1.0f/det;
    const V3 s = o - t.v0;
    u = dot(s, p) * inv;
    if (u < -1e-5f || u > 1.0f+1e-5f) return -1.0f;
    const V3 q = cross(s, e1);
    v = dot(d, q) * inv;
    if (v < -1e-5f || u+v > 1.0f+1e-5f) return -1.0f;
    const float tt = dot(e2, q) * inv;
    return (tt > 1e-7f) ? tt : -1.0f;
}

int closestHit(const BVH& bvh, const V3& o, const V3& d, float tMax,
               float& outT, float& outU, float& outV) {
    const V3 invDir = { 1.0f/d.x, 1.0f/d.y, 1.0f/d.z };
    int best = -1; outT = tMax;
    int stack[128], sp = 0; stack[sp++] = 0;
    while (sp) {
        const BVHNode& n = bvh.nodes[stack[--sp]];
        if (!hitAABB(n.box, o, invDir, outT)) continue;
        if (n.count > 0) {
            for (int k = 0; k < n.count; ++k) {
                const int ti = bvh.order[n.start+k];
                float u, v; const float t = rayTri(o, d, bvh.tris[ti], u, v);
                if (t > 0.0f && t < outT) { outT=t; outU=u; outV=v; best=ti; }
            }
        } else {
            if (n.left  >= 0) stack[sp++] = n.left;
            if (n.right >= 0) stack[sp++] = n.right;
        }
    }
    return best;
}

// Tangentes par sommet (Lengyel) a partir de positions+UV+normales.
void computeTangents(const float* pos, const float* nrm, const float* uv,
                     size_t vcount, const unsigned int* idx, size_t icount,
                     std::vector<float>& out4) {
    out4.assign(vcount*4, 0.0f);
    std::vector<V3> tan(vcount, {0,0,0}), bit(vcount, {0,0,0});
    for (size_t i = 0; i+2 < icount; i += 3) {
        const unsigned int a=idx[i], b=idx[i+1], c=idx[i+2];
        const V3 p0=getV3(pos,a), p1=getV3(pos,b), p2=getV3(pos,c);
        const float u0=uv[a*2],v0=uv[a*2+1],u1=uv[b*2],v1=uv[b*2+1],u2=uv[c*2],v2=uv[c*2+1];
        const V3 e1=p1-p0, e2=p2-p0;
        const float du1=u1-u0, dv1=v1-v0, du2=u2-u0, dv2=v2-v0;
        const float denom = du1*dv2 - du2*dv1;
        if (std::fabs(denom) < 1e-12f) continue;
        const float r = 1.0f/denom;
        const V3 sdir=(e1*dv2-e2*dv1)*r, tdir=(e2*du1-e1*du2)*r;
        for (unsigned int k : { a,b,c }) { tan[k]=tan[k]+sdir; bit[k]=bit[k]+tdir; }
    }
    for (size_t i = 0; i < vcount; ++i) {
        const V3 n=getV3(nrm,i), raw=tan[i];
        const V3 t = norm(raw - n*dot(n,raw));
        const float w = (dot(cross(n,t), bit[i]) < 0.0f) ? -1.0f : 1.0f;
        out4[i*4+0]=t.x; out4[i*4+1]=t.y; out4[i*4+2]=t.z; out4[i*4+3]=w;
    }
}

} // namespace

static void encodePng(const std::vector<float>& img, const std::vector<uint8_t>& mask,
                      int W, int H, MapKind kind, std::vector<unsigned char>& outPng) {
    std::vector<uint8_t> rgb(static_cast<size_t>(W)*H*3);
    for (size_t i = 0; i < (size_t)W*H; ++i) {
        V3 px;
        if (mask[i]) px = { img[i*3], img[i*3+1], img[i*3+2] };
        else px = (kind == MapKind::NormalTangent) ? V3{0,0,1} : V3{1,1,1};
        for (int c = 0; c < 3; ++c) {
            float v = px.x; if (c==1) v=px.y; else if (c==2) v=px.z;
            const float enc = (kind == MapKind::NormalTangent) ? (v*0.5f+0.5f) : v;
            rgb[i*3+c] = (uint8_t)std::clamp((int)std::lround(enc*255.0f), 0, 255);
        }
    }
    outPng.clear();
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size){
            auto* vec = static_cast<std::vector<unsigned char>*>(ctx);
            const uint8_t* p = static_cast<uint8_t*>(data);
            vec->insert(vec->end(), p, p+size);
        }, &outPng, W, H, 3, rgb.data(), W*3);
}

// --------------------------------------------------------------------------
bool bakeMaps(const BakeHigh& high, const BakeLow& low,
              const SrcMap* sources, int nSources,
              int res, float cageScale, BakeMapsResult& out) {
    if (!low.uv || low.icount < 3 || high.icount < 3 || nSources <= 0) return false;
    if (res < 4) res = 4;

    AABB bb;
    for (size_t i = 0; i < high.vcount; ++i) bb.grow(getV3(high.pos, i));
    const V3 ext = bb.mx - bb.mn;
    const float diag = std::sqrt(dot(ext, ext));
    const float cage = std::max(diag * cageScale, 1e-5f);

    BVH bvh;
    bvh.buildFrom(high);
    if (bvh.tris.empty()) return false;

    // Decode des sources
    std::vector<SrcImage> imgs(nSources);
    bool anyNormalImg = false;
    for (int s = 0; s < nSources; ++s) {
        imgs[s].load(sources[s].png, sources[s].size);
        if (sources[s].kind == MapKind::NormalTangent && imgs[s].valid() && high.uv)
            anyNormalImg = true;
    }

    // Tangentes : LOW (toujours), HIGH (si une normal map detail est compositee)
    computeTangents(low.pos, low.nrm, low.uv, low.vcount, low.idx, low.icount, out.tangents);
    std::vector<float> hTan;
    if (anyNormalImg)
        computeTangents(high.pos, high.nrm, high.uv, high.vcount, high.idx, high.icount, hTan);

    const int W = res, H = res;
    std::vector<std::vector<float>> chan(nSources, std::vector<float>((size_t)W*H*3, 0.0f));
    std::vector<uint8_t> mask((size_t)W*H, 0);
    out.texelsHit = 0; out.texelsTotal = 0;

    for (size_t i = 0; i+2 < low.icount; i += 3) {
        const unsigned int a=low.idx[i], b=low.idx[i+1], c=low.idx[i+2];
        float au=low.uv[a*2],av=low.uv[a*2+1],bu=low.uv[b*2],bv=low.uv[b*2+1],cu=low.uv[c*2],cv=low.uv[c*2+1];
        const float offU = std::floor(std::min({au,bu,cu}));
        const float offV = std::floor(std::min({av,bv,cv}));
        au-=offU;bu-=offU;cu-=offU; av-=offV;bv-=offV;cv-=offV;
        const float ax=au*W, ay=av*H, bx=bu*W, by=bv*H, cx=cu*W, cy=cv*H;

        int minX=std::max(0,(int)std::floor(std::min({ax,bx,cx})));
        int maxX=std::min(W-1,(int)std::ceil(std::max({ax,bx,cx})));
        int minY=std::max(0,(int)std::floor(std::min({ay,by,cy})));
        int maxY=std::min(H-1,(int)std::ceil(std::max({ay,by,cy})));

        const float area=(bx-ax)*(cy-ay)-(cx-ax)*(by-ay);
        if (std::fabs(area) < 1e-9f) continue;
        const float invArea = 1.0f/area;

        const V3 p0=getV3(low.pos,a),p1=getV3(low.pos,b),p2=getV3(low.pos,c);
        const V3 n0=getV3(low.nrm,a),n1=getV3(low.nrm,b),n2=getV3(low.nrm,c);
        const V3 t0={out.tangents[a*4],out.tangents[a*4+1],out.tangents[a*4+2]};
        const V3 t1={out.tangents[b*4],out.tangents[b*4+1],out.tangents[b*4+2]};
        const V3 t2={out.tangents[c*4],out.tangents[c*4+1],out.tangents[c*4+2]};
        const float wHand = out.tangents[a*4+3];

        for (int y=minY; y<=maxY; ++y)
        for (int x=minX; x<=maxX; ++x) {
            const float px=x+0.5f, py=y+0.5f;
            const float l0=((bx-px)*(cy-py)-(cx-px)*(by-py))*invArea;
            const float l1=((cx-px)*(ay-py)-(ax-px)*(cy-py))*invArea;
            const float l2=1.0f-l0-l1;
            const float e=-1e-4f;
            if (l0<e||l1<e||l2<e) continue;

            const V3 P = p0*l0+p1*l1+p2*l2;
            const V3 N = norm(n0*l0+n1*l1+n2*l2);
            V3 T = t0*l0+t1*l1+t2*l2;
            T = norm(T - N*dot(N,T));
            const V3 B = cross(N,T) * wHand;

            const V3 o = P + N*cage;
            const V3 d = N * -1.0f;
            float t,u,v;
            ++out.texelsTotal;
            const size_t texel = (size_t)y*W + x;

            // UV bas-poly interpole (fallback de sampling)
            const float lu = low.uv[a*2]*l0 + low.uv[b*2]*l1 + low.uv[c*2]*l2;
            const float lv = low.uv[a*2+1]*l0 + low.uv[b*2+1]*l1 + low.uv[c*2+1]*l2;

            const int hit = closestHit(bvh, o, d, cage*2.0f, t, u, v);
            bool goodHit = false;
            V3 Nh{0,0,1}, Th{1,0,0}, Bh{0,1,0};
            float hu = lu, hv = lv;
            if (hit >= 0) {
                const Tri& tr = bvh.tris[hit];
                const float w0=1-u-v, w1=u, w2=v;
                Nh = norm(getV3(high.nrm,tr.i0)*w0 + getV3(high.nrm,tr.i1)*w1 + getV3(high.nrm,tr.i2)*w2);
                goodHit = dot(Nh, N) > 0.173f;
                if (goodHit) {
                    hu = high.uv[tr.i0*2]*w0 + high.uv[tr.i1*2]*w1 + high.uv[tr.i2*2]*w2;
                    hv = high.uv[tr.i0*2+1]*w0 + high.uv[tr.i1*2+1]*w1 + high.uv[tr.i2*2+1]*w2;
                    if (anyNormalImg) {
                        const V3 T0={hTan[tr.i0*4],hTan[tr.i0*4+1],hTan[tr.i0*4+2]};
                        const V3 T1={hTan[tr.i1*4],hTan[tr.i1*4+1],hTan[tr.i1*4+2]};
                        const V3 T2={hTan[tr.i2*4],hTan[tr.i2*4+1],hTan[tr.i2*4+2]};
                        Th = norm((T0*w0+T1*w1+T2*w2) - Nh*dot(Nh, T0*w0+T1*w1+T2*w2));
                        Bh = cross(Nh,Th) * hTan[tr.i0*4+3];
                    }
                }
            }
            if (goodHit) ++out.texelsHit;

            for (int s = 0; s < nSources; ++s) {
                V3 val;
                if (sources[s].kind == MapKind::Color) {
                    val = imgs[s].valid() ? imgs[s].sampleColor(goodHit ? hu : lu, goodHit ? hv : lv)
                                          : V3{1,1,1};
                } else { // NormalTangent
                    V3 objN = N;
                    if (goodHit) {
                        if (imgs[s].valid()) {
                            const V3 det = imgs[s].sampleNormal(hu, hv);
                            objN = norm(Th*det.x + Bh*det.y + Nh*det.z);
                        } else objN = Nh;
                    } else if (imgs[s].valid()) {
                        const V3 det = imgs[s].sampleNormal(lu, lv);
                        objN = norm(T*det.x + B*det.y + N*det.z);
                    }
                    val = norm(V3{ dot(objN,T), dot(objN,B), dot(objN,N) });
                }
                chan[s][texel*3+0]=val.x; chan[s][texel*3+1]=val.y; chan[s][texel*3+2]=val.z;
            }
            mask[texel]=1;
        }
    }

    // Dilatation commune (meme couverture pour toutes les maps)
    for (int pass = 0; pass < 8; ++pass) {
        std::vector<uint8_t> m2 = mask;
        for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t idx=(size_t)y*W+x;
            if (mask[idx]) continue;
            int cnt=0; std::vector<float> acc(nSources*3, 0.0f);
            for (int dy=-1; dy<=1; ++dy)
            for (int dx=-1; dx<=1; ++dx) {
                const int nx=x+dx, ny=y+dy;
                if (nx<0||ny<0||nx>=W||ny>=H) continue;
                const size_t ni=(size_t)ny*W+nx;
                if (!mask[ni]) continue;
                for (int s=0;s<nSources;++s){ acc[s*3]+=chan[s][ni*3];acc[s*3+1]+=chan[s][ni*3+1];acc[s*3+2]+=chan[s][ni*3+2]; }
                ++cnt;
            }
            if (cnt){ for (int s=0;s<nSources;++s){ chan[s][idx*3]=acc[s*3]/cnt;chan[s][idx*3+1]=acc[s*3+1]/cnt;chan[s][idx*3+2]=acc[s*3+2]/cnt; } m2[idx]=1; }
        }
        mask.swap(m2);
    }

    out.maps.resize(nSources);
    for (int s = 0; s < nSources; ++s)
        encodePng(chan[s], mask, W, H, sources[s].kind, out.maps[s].png);
    return true;
}

// --------------------------------------------------------------------------
// Wrapper : bake d'une seule normal map (chemin LOD standard, UV preservees).
bool bakeNormalMap(const BakeHigh& high, const BakeLow& low,
                   const unsigned char* srcPng, size_t srcPngSize,
                   int res, float cageScale, BakeResult& out) {
    SrcMap src{ srcPng, srcPngSize, MapKind::NormalTangent };
    BakeMapsResult r;
    if (!bakeMaps(high, low, &src, 1, res, cageScale, r) || r.maps.empty()) return false;
    out.tangents     = std::move(r.tangents);
    out.png          = std::move(r.maps[0].png);
    out.width = res; out.height = res;
    out.texelsHit = r.texelsHit; out.texelsTotal = r.texelsTotal;
    out.usedSourceMap = (srcPng && srcPngSize > 0);
    return !out.png.empty();
}
