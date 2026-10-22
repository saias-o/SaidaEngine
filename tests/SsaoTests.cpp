// AmbientOcclusionPass's SSAO (shaders/ssao.glsl) compiled as C++ over GLM:
// these checks run the code the GPU runs, on depth buffers ray-cast from known
// geometry through the engine's projections and the pass's push constants. No
// window, device or GPU is involved.

#include "core/Camera.hpp"
#include "render/AmbientOcclusionPass.hpp"
#include "scene/Scene.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace ssao_shader {
using namespace glm;
float sampleDepth(vec2 viewportUV);
float sampleRawVisibility(vec2 viewportUV);
#include "ssao_denoise.glsl"
}  // namespace ssao_shader

using namespace saida;
using ssao_shader::SsaoInputs;

namespace {
int checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        std::cerr << "[ssao] FAIL: " << message << '\n';
        std::abort();
    }
}

std::string describe(const char* what, double value) {
    std::ostringstream text;
    text << what << " (measured " << value << ")";
    return text.str();
}

constexpr int kWidth = 192;
constexpr int kHeight = 108;
constexpr int kNoObject = -1;

// An axis-aligned box; zero thickness on one axis makes it a rectangle.
struct Box {
    glm::vec3 lo;
    glm::vec3 hi;
};

struct Eye {
    glm::mat4 view;
    glm::mat4 projection;
};

// What the depth pre-pass would leave: reversed depth (0 where nothing was
// drawn), and as the oracle, which box each pixel sees and that box's world
// normal. `raw` is what the kernel stage then leaves (runKernel).
struct DepthImage {
    int width = kWidth;
    int height = kHeight;
    std::vector<float> depth;
    std::vector<int> object;
    std::vector<glm::vec3> worldPoint;
    std::vector<glm::vec3> worldNormal;
    std::vector<float> raw;
    size_t index(int x, int y) const { return size_t(y) * size_t(width) + size_t(x); }
};

// Slab test: the ray's entry into `box` in front of `origin`, and the entry face's normal.
bool intersect(const Box& box, glm::vec3 origin, glm::vec3 dir, float& t, glm::vec3& normal) {
    float tNear = -std::numeric_limits<float>::infinity();
    float tFar = std::numeric_limits<float>::infinity();
    int entryAxis = -1;
    for (int axis = 0; axis < 3; ++axis) {
        if (dir[axis] == 0.0f) {
            if (origin[axis] < box.lo[axis] || origin[axis] > box.hi[axis]) return false;
            continue;
        }
        float t0 = (box.lo[axis] - origin[axis]) / dir[axis];
        float t1 = (box.hi[axis] - origin[axis]) / dir[axis];
        if (t0 > t1) std::swap(t0, t1);
        if (t0 > tNear) {
            tNear = t0;
            entryAxis = axis;
        }
        tFar = std::min(tFar, t1);
    }
    if (entryAxis < 0 || tNear > tFar || tNear <= 0.0f) return false;
    t = tNear;
    normal = glm::vec3(0.0f);
    normal[entryAxis] = dir[entryAxis] > 0.0f ? -1.0f : 1.0f;
    return true;
}

DepthImage render(const std::vector<Box>& scene, const Eye& eye) {
    DepthImage image;
    const size_t count = size_t(image.width) * size_t(image.height);
    image.depth.assign(count, 0.0f);
    image.object.assign(count, kNoObject);
    image.worldPoint.assign(count, glm::vec3(0.0f));
    image.worldNormal.assign(count, glm::vec3(0.0f));
    const glm::mat4 viewProjection = eye.projection * eye.view;
    const glm::mat4 toWorld = glm::inverse(viewProjection);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const glm::vec2 uv = (glm::vec2(x, y) + 0.5f) / glm::vec2(image.width, image.height);
            const glm::vec2 ndc = uv * 2.0f - 1.0f;
            const glm::vec4 nearPoint = toWorld * glm::vec4(ndc, 1.0f, 1.0f);  // reversed: near is 1
            const glm::vec4 farPoint = toWorld * glm::vec4(ndc, 0.0f, 1.0f);
            const glm::vec3 origin = glm::vec3(nearPoint) / nearPoint.w;
            const glm::vec3 dir = glm::vec3(farPoint) / farPoint.w - origin;
            float bestT = std::numeric_limits<float>::infinity();
            const size_t i = image.index(x, y);
            for (size_t b = 0; b < scene.size(); ++b) {
                float t = 0.0f;
                glm::vec3 normal(0.0f);
                if (intersect(scene[b], origin, dir, t, normal) && t < bestT) {
                    bestT = t;
                    image.object[i] = int(b);
                    image.worldNormal[i] = normal;
                }
            }
            if (image.object[i] == kNoObject) continue;
            const glm::vec3 point = origin + dir * bestT;
            const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
            image.depth[i] = clip.z / clip.w;
            image.worldPoint[i] = point;
        }
    }
    return image;
}

// The image ssao_shader::sampleDepth and sampleRawVisibility read, for the
// duration of one shader call.
const DepthImage* sampled = nullptr;

class Sampling {
public:
    explicit Sampling(const DepthImage& image) { sampled = &image; }
    ~Sampling() { sampled = nullptr; }
    Sampling(const Sampling&) = delete;
    Sampling& operator=(const Sampling&) = delete;
};

glm::vec2 pixelUV(const DepthImage& image, int x, int y) {
    return (glm::vec2(x, y) + 0.5f) / glm::vec2(image.width, image.height);
}

SsaoInputs inputsFor(const Eye& eye, const DepthImage& image, const SceneSettings& settings = {}) {
    const AmbientOcclusionPass::PushConstants push =
        AmbientOcclusionPass::pushConstants(settings, eye.projection);
    SsaoInputs inputs;
    inputs.settings = push.settings;
    inputs.invProjection = push.invProjection;
    inputs.invProjection2 = push.invProjection2;
    inputs.invOffsets = push.invOffsets;
    inputs.pixels = glm::vec2(image.width, image.height);
    return inputs;
}

// The kernel stage's output at one texel.
float rawVisibilityAt(const DepthImage& image, const SsaoInputs& inputs, int x, int y) {
    const Sampling sampling(image);
    return ssao_shader::ssaoRawVisibility(inputs, pixelUV(image, x, y));
}

// The kernel stage over the whole image, as the first AO pass leaves it.
void runKernel(DepthImage& image, const SsaoInputs& inputs) {
    image.raw.assign(image.depth.size(), 1.0f);
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x)
            image.raw[image.index(x, y)] = rawVisibilityAt(image, inputs, x, y);
}

// What the scene pass reads at a texel: the denoise stage over runKernel's output.
float visibilityAt(const DepthImage& image, const SsaoInputs& inputs, int x, int y) {
    if (image.raw.size() != image.depth.size()) {
        std::cerr << "[ssao] FAIL: the denoise stage read an image the kernel stage never ran on\n";
        std::abort();
    }
    const Sampling sampling(image);
    return ssao_shader::ssaoDenoise(inputs, pixelUV(image, x, y));
}

glm::vec3 shaderNormalAt(const DepthImage& image, const SsaoInputs& inputs, int x, int y) {
    const Sampling sampling(image);
    const glm::vec2 uv = pixelUV(image, x, y);
    const float depth = image.depth[image.index(x, y)];
    const glm::vec3 centre = ssao_shader::ssaoViewPosition(inputs, uv, depth);
    return ssao_shader::ssaoSurfaceNormal(inputs, uv, centre, depth);
}

glm::mat4 desktopProjection() {
    Camera camera;
    camera.setPerspective(glm::radians(70.0f), float(kWidth) / float(kHeight), 0.05f, 500.0f);
    return camera.projection();
}

// An off-centre frustum shaped like an OpenXR eye's (tangents of its four
// half-angles; left and down negative), with reversed depth and Vulkan's
// Y-down clip space.
glm::mat4 eyeProjection(float tanLeft, float tanRight, float tanUp, float tanDown) {
    constexpr float nearZ = 0.03f, farZ = 300.0f;
    const float width = tanRight - tanLeft;
    const float height = tanDown - tanUp;
    glm::mat4 m(0.0f);
    m[0][0] = 2.0f / width;
    m[1][1] = 2.0f / height;
    m[2][0] = (tanRight + tanLeft) / width;
    m[2][1] = (tanUp + tanDown) / height;
    m[2][2] = nearZ / (farZ - nearZ);
    m[2][3] = -1.0f;
    m[3][2] = farZ * nearZ / (farZ - nearZ);
    return m;
}

const glm::mat4 kLeftEye = eyeProjection(std::tan(-0.96f), std::tan(0.72f), std::tan(0.85f), std::tan(-0.74f));
const glm::mat4 kRightEye = eyeProjection(std::tan(-0.72f), std::tan(0.96f), std::tan(0.85f), std::tan(-0.74f));

glm::mat4 orthographicProjection() {
    return glm::orthoRH_ZO(-3.0f, 5.0f, -2.0f, 2.5f, 400.0f, 0.05f);  // far/near swapped: reversed
}

glm::mat4 lookFrom(glm::vec3 position, glm::vec3 target) {
    return glm::lookAt(position, target, glm::vec3(0.0f, 1.0f, 0.0f));
}

const Box kFloor{{-300.0f, -1.0f, -300.0f}, {300.0f, 0.0f, 300.0f}};

void testFlatFloorNeverOccludesItself() {
    const std::vector<glm::mat4> projections{desktopProjection(), kLeftEye, kRightEye,
                                             orthographicProjection()};
    long floorPixels = 0;
    float worst = 1.0f;
    for (const glm::mat4& projection : projections) {
        for (float pitch : {-6.0f, -18.0f, -40.0f, -75.0f}) {
            // Sub-pixel steps: a kernel that reads depth at one point and
            // rebuilds the surface at another fails between texels, not on them.
            for (int shift = 0; shift < 6; ++shift) {
                const float yaw = glm::radians(0.37f * float(shift));
                const float p = glm::radians(pitch);
                const glm::vec3 position(0.0011f * float(shift), 1.6f, -0.0023f * float(shift));
                const glm::vec3 forward(std::cos(p) * std::sin(yaw), std::sin(p), -std::cos(p) * std::cos(yaw));
                const Eye eye{lookFrom(position, position + forward), projection};
                DepthImage image = render({kFloor}, eye);
                const SsaoInputs inputs = inputsFor(eye, image);
                runKernel(image, inputs);
                for (int y = 0; y < image.height; ++y) {
                    for (int x = 0; x < image.width; ++x) {
                        if (image.object[image.index(x, y)] == kNoObject) continue;
                        ++floorPixels;
                        worst = std::min(worst, visibilityAt(image, inputs, x, y));
                    }
                }
            }
        }
    }
    require(floorPixels > 100000, "the floor views cover the floor");
    require(worst == 1.0f, describe("a flat floor never occludes itself, at any pitch, sub-pixel "
                                    "offset or projection; lowest visibility", worst));
}

// A rectangle 0.8 x 0.6 m in front of either a wall `wallGap` behind it or
// nothing at all, seen obliquely so that its depth changes across the screen.
struct PlateScene {
    std::vector<Box> boxes;
    static constexpr int kPlate = 0;
};

PlateScene plateScene(float wallGap) {
    PlateScene scene;
    scene.boxes.push_back({{-0.4f, 0.9f, 0.0f}, {0.4f, 1.5f, 0.0f}});
    if (wallGap > 0.0f)
        scene.boxes.push_back({{-60.0f, -60.0f, -wallGap}, {60.0f, 60.0f, -wallGap}});
    return scene;
}

const std::vector<glm::vec3> kPlateViewpoints{
    {0.9f, 1.6f, 2.0f}, {-0.8f, 0.9f, 1.7f}, {0.2f, 2.2f, 1.4f}, {-0.3f, 1.25f, 2.4f}};

// A table top 0.75 m above the floor -- the AO radius -- with a box on it:
// creases where the box meets the top, and between each one's faces.
// Samples that fall off the table's edge are rejected, those on the box
// occlude, and near the table's corner few samples survive. `topReach`
// extends the top (not its legs) past the original edge.
constexpr int kTable = 1;
constexpr float kTableEdge = 0.6f;

std::vector<Box> tableScene(float topReach = kTableEdge) {
    std::vector<Box> boxes{kFloor,
                           {{-0.6f, 0.0f, -0.4f}, {kTableEdge, 0.75f, 0.4f}},
                           {{0.1f, 0.75f, -0.1f}, {0.3f, 1.15f, 0.1f}},
                           {{-3.0f, 0.0f, -1.6f}, {3.0f, 2.5f, -1.5f}}};
    if (topReach > kTableEdge) boxes.push_back({{kTableEdge, 0.75f, -0.4f}, {topReach, 0.75f, 0.4f}});
    return boxes;
}

const std::vector<glm::vec3> kTableViewpoints{{1.6f, 1.6f, 0.8f}, {0.9f, 1.9f, 1.5f}, {-0.4f, 1.5f, 1.3f}};
const glm::vec3 kTableTarget(0.3f, 0.8f, 0.0f);

bool sameFace(const DepthImage& image, size_t a, size_t b) {
    return image.object[a] == image.object[b] && image.worldNormal[a] == image.worldNormal[b];
}

bool inside(const DepthImage& image, int x, int y) {
    return x >= 0 && y >= 0 && x < image.width && y < image.height;
}

// Whether, on each screen axis, the pixel's face continues over both
// neighbours on at least one side: what the estimate needs to confirm the
// side it takes. A face one or two texels thick on an axis -- a corner's tip,
// a box top at a grazing angle -- leaves the far neighbour on a third surface
// at a crease, where the evidence of the two sides cannot be told apart.
bool faceSpansThreeTexels(const DepthImage& image, int x, int y) {
    auto same = [&](int dx, int dy) {
        return inside(image, x + dx, y + dy) && sameFace(image, image.index(x, y), image.index(x + dx, y + dy));
    };
    return ((same(-1, 0) && same(-2, 0)) || (same(1, 0) && same(2, 0))) &&
           ((same(0, -1) && same(0, -2)) || (same(0, 1) && same(0, 2)));
}

// Whether every texel of the same face in the denoise stage's block around
// (x, y) -- whose raw values it averages -- spans three texels too.
bool blockSpansThreeTexels(const DepthImage& image, int x, int y) {
    const int half = ssao_shader::AO_PATTERN / 2;
    for (int dy = -half; dy < half; ++dy)
        for (int dx = -half; dx < half; ++dx)
            if (inside(image, x + dx, y + dy) &&
                sameFace(image, image.index(x, y), image.index(x + dx, y + dy)) &&
                !faceSpansThreeTexels(image, x + dx, y + dy))
                return false;
    return true;
}

struct NormalErrors {
    float worstDegrees = 0.0f;
    long edgePixels = 0;  // a neighbour shows another face: a silhouette or a crease
    long thin = 0;
    bool unitAndFinite = true;
};

void measureNormals(const std::vector<Box>& boxes, const Eye& eye, NormalErrors& errors) {
    const DepthImage image = render(boxes, eye);
    const SsaoInputs inputs = inputsFor(eye, image);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const size_t i = image.index(x, y);
            if (image.object[i] == kNoObject) continue;
            bool edge = false;
            for (const glm::ivec2 n : {glm::ivec2(x - 1, y), glm::ivec2(x + 1, y), glm::ivec2(x, y - 1),
                                       glm::ivec2(x, y + 1)})
                edge = edge || (inside(image, n.x, n.y) && !sameFace(image, i, image.index(n.x, n.y)));
            if (edge) ++errors.edgePixels;
            const glm::vec3 normal = shaderNormalAt(image, inputs, x, y);
            errors.unitAndFinite = errors.unitAndFinite && std::abs(glm::length(normal) - 1.0f) < 1e-5f;
            if (!faceSpansThreeTexels(image, x, y)) {
                ++errors.thin;
                continue;
            }
            const glm::vec3 expected = glm::normalize(glm::mat3(eye.view) * image.worldNormal[i]);
            const float cosine = std::clamp(glm::dot(normal, expected), -1.0f, 1.0f);
            errors.worstDegrees = std::max(errors.worstDegrees, glm::degrees(std::acos(cosine)));
        }
    }
}

void testNormalsStayOnTheirSurface() {
    NormalErrors errors;
    for (const glm::mat4& projection : {desktopProjection(), kLeftEye, kRightEye}) {
        for (float wallGap : {0.0f, 0.3f, 3.5f})
            for (const glm::vec3& position : kPlateViewpoints)
                measureNormals(plateScene(wallGap).boxes,
                               {lookFrom(position, glm::vec3(0.0f, 1.2f, 0.0f)), projection}, errors);
        for (const glm::vec3& position : kTableViewpoints)
            measureNormals(tableScene(), {lookFrom(position, kTableTarget), projection}, errors);
    }
    std::cout << "[ssao] normals: " << errors.edgePixels << " silhouette and crease pixels, " << errors.thin
              << " on faces under three texels thick, worst " << errors.worstDegrees << " degrees elsewhere\n";
    require(errors.edgePixels > 5000, "the views include silhouettes and creases");
    require(errors.thin * 10 < errors.edgePixels, "faces too thin to confirm are a small share of the edges");
    require(errors.unitAndFinite, "every reconstructed normal is a finite unit vector, thin faces included");
    require(errors.worstDegrees < 0.1f,
            describe("the normal at a silhouette (against sky or a nearer or farther wall) or at a "
                     "crease is its own face's; worst error in degrees", errors.worstDegrees));
}

void testSilhouettesNeitherDarkenNorHalo() {
    float worstPlate = 1.0f;
    float worstThin = 1.0f;
    float worstFarWall = 1.0f;
    bool finite = true;
    for (float wallGap : {0.0f, 0.3f, 3.5f}) {
        const PlateScene scene = plateScene(wallGap);
        for (const glm::mat4& projection : {desktopProjection(), kLeftEye, kRightEye}) {
            for (const glm::vec3& position : kPlateViewpoints) {
                const Eye eye{lookFrom(position, glm::vec3(0.0f, 1.2f, 0.0f)), projection};
                DepthImage image = render(scene.boxes, eye);
                const SsaoInputs inputs = inputsFor(eye, image);
                runKernel(image, inputs);
                for (int y = 0; y < image.height; ++y) {
                    for (int x = 0; x < image.width; ++x) {
                        const float visibility = visibilityAt(image, inputs, x, y);
                        finite = finite && std::isfinite(visibility) && visibility >= 0.0f && visibility <= 1.0f;
                        const int object = image.object[image.index(x, y)];
                        if (object == PlateScene::kPlate) {
                            if (blockSpansThreeTexels(image, x, y)) worstPlate = std::min(worstPlate, visibility);
                            else worstThin = std::min(worstThin, visibility);
                        } else if (object != kNoObject && wallGap > 1.0f) {
                            worstFarWall = std::min(worstFarWall, visibility);
                        }
                    }
                }
            }
        }
    }
    std::cout << "[ssao] lowest visibility on plate texels averaging a texel too thin to confirm: " << worstThin << '\n';
    require(finite, "visibility is finite and within [0, 1] at every pixel, edges included");
    require(worstPlate == 1.0f, describe("a flat plate is unoccluded up to its silhouette; lowest "
                                         "visibility", worstPlate));
    require(worstFarWall == 1.0f, describe("a wall further behind the plate than the AO radius gets "
                                           "no halo; lowest visibility", worstFarWall));
}

void testDegenerateTangentsFallBackToTheViewer() {
    using glm::vec3;
    using glm::vec4;
    const vec3 toward = glm::normalize(vec3(0.2f, -0.1f, 1.0f));
    const vec3 cases[][2] = {
        {vec3(0.0f), vec3(0.0f)},
        {vec3(0.01f, 0.0f, 0.0f), vec3(0.0f)},
        {vec3(0.01f, 0.0f, 0.0f), vec3(0.03f, 0.0f, 0.0f)},
        {vec3(0.01f, 0.0f, 0.0f), vec3(-0.02f, 0.0f, 0.0f)},
        {vec3(0.01f, 0.0f, 0.0f), vec3(0.03f, 1e-6f, 0.0f)},
        {vec3(1e-30f, 0.0f, 0.0f), vec3(0.0f, 1e-30f, 0.0f)},
    };
    for (const auto& tangents : cases) {
        const vec3 normal = ssao_shader::ssaoNormal(tangents[0], tangents[1], toward);
        require(normal == toward, "tangents that span no plane give the viewer's direction, not NaN");
    }
    const vec3 flipped = ssao_shader::ssaoNormal(vec3(0.0f, 0.01f, 0.0f), vec3(0.01f, 0.0f, 0.0f), toward);
    require(std::abs(glm::length(flipped) - 1.0f) < 1e-6f && glm::dot(flipped, toward) > 0.0f &&
                std::abs(std::abs(flipped.z) - 1.0f) < 1e-6f,
            "a valid tangent pair gives the unit normal facing the viewer");

}

// Which side's near neighbour continues the centre's surface, from depths
// alone (reversed: larger is nearer, 0 nothing drawn). The centre is at 0.5
// on a plane whose depth rises by kSlope per texel along the axis; the values
// are exact in binary, so a plane's extrapolation misses by exactly zero.
void testSameSurfaceSideFromDepth() {
    using ssao_shader::ssaoAfterIsSameSurface;
    constexpr float c = 0.5f;
    constexpr float kSlope = 1.0f / 128.0f;
    constexpr float kOtherFace = 3.0f / 1024.0f;  // a steeper-sided face's smaller first step
    require(ssaoAfterIsSameSurface(c, c - kSlope, c - 2 * kSlope, c + kSlope, c + 2 * kSlope),
            "on a plane both sides continue it and the side after is taken");
    require(!ssaoAfterIsSameSurface(c, c - kSlope, c - 2 * kSlope, c + kOtherFace, c + 2 * kOtherFace - 1.0f / 1024.0f),
            "at a crease, the face across it is refused although its step is the smaller");
    require(ssaoAfterIsSameSurface(c, c - kOtherFace, c - 2 * kOtherFace + 1.0f / 1024.0f, c + kSlope, c + 2 * kSlope),
            "at a crease on the other side, the same holds mirrored");
    require(!ssaoAfterIsSameSurface(c, c - kSlope, c - 2 * kSlope, 0.2f, 0.2f),
            "past a silhouette the background is refused");
    require(ssaoAfterIsSameSurface(c, 0.2f, 0.2f, c + kSlope, 0.2f),
            "a strip two texels wide is kept by its small step though its far neighbour is off it");
    require(!ssaoAfterIsSameSurface(c, c - kSlope, c - 2 * kSlope, c + kSlope, 0.0f),
            "a side whose far neighbour is missing loses to a side that continues the plane");
    require(ssaoAfterIsSameSurface(c, 0.2f, 0.2f, c + kSlope, 0.0f),
            "it still wins against a silhouette, by its own step");
    require(!ssaoAfterIsSameSurface(c, 0.2f, 0.2f, 0.0f, 0.0f),
            "a side without its near neighbour is never taken");
    require(ssaoAfterIsSameSurface(c, 0.0f, 0.0f, 0.2f, 0.2f),
            "the only side with a neighbour is taken, however far its depth");
}

// Whether every texel within `reach` of (x, y) shows that texel's face: away
// from silhouettes and creases, where the denoise stage's average changes
// composition, and where a step in the visibility can only be the kernel's.
bool faceAround(const DepthImage& image, int x, int y, int reach) {
    for (int dy = -reach; dy <= reach; ++dy)
        for (int dx = -reach; dx <= reach; ++dx)
            if (!inside(image, x + dx, y + dy) ||
                !sameFace(image, image.index(x, y), image.index(x + dx, y + dy)))
                return false;
    return true;
}

// The occlusion integral, as a kernel 32 times denser than the GPU's measures
// it: the same estimator, 64 directions and 16 steps, the same for every texel.
float denseVisibilityAt(const DepthImage& image, const SsaoInputs& inputs, int x, int y) {
    const Sampling sampling(image);
    return std::pow(ssao_shader::ssaoKernelVisibility(inputs, pixelUV(image, x, y), 64, 16, 0.5f, 0.5f),
                    inputs.settings.w);
}

// A kernel the same for every texel samples the occlusion integral at the
// same sixteen places everywhere, and prints one hard-edged copy of each
// occluder per sample: bands on every floor and wall near a contact. What the
// scene pass reads must follow the integral instead -- within each face, away
// from its edges, close to what the dense kernel measures.
void testOcclusionFollowsTheIntegral() {
    double total = 0.0;
    float largest = 0.0f;
    float lowest = 1.0f;
    long texels = 0;
    for (const glm::mat4& projection : {desktopProjection(), kLeftEye}) {
        for (const glm::vec3& position : kTableViewpoints) {
            const Eye eye{lookFrom(position, kTableTarget), projection};
            DepthImage image = render(tableScene(), eye);
            const SsaoInputs inputs = inputsFor(eye, image);
            runKernel(image, inputs);
            for (int y = 0; y < image.height; ++y) {
                for (int x = 0; x < image.width; ++x) {
                    if (image.object[image.index(x, y)] == kNoObject || !faceAround(image, x, y, 3)) continue;
                    const float dense = denseVisibilityAt(image, inputs, x, y);
                    const float error = std::abs(visibilityAt(image, inputs, x, y) - dense);
                    total += error;
                    largest = std::max(largest, error);
                    lowest = std::min(lowest, dense);
                    ++texels;
                }
            }
        }
    }
    const double mean = total / double(std::max(texels, 1L));
    std::cout << "[ssao] integral: " << texels << " texels, mean error " << mean << ", largest " << largest
              << ", lowest " << lowest << '\n';
    require(texels > 20000, "the views cover the table's faces");
    require(lowest < 0.9f, describe("the faces include real occlusion; lowest visibility", lowest));
    // A kernel the same for every texel, denoised alike, stays twice as far.
    constexpr double kMeanError = 0.001;
    constexpr float kLargestError = 0.03f;
    require(mean < kMeanError, describe("the visibility follows the occlusion integral; mean error", mean));
    require(largest < kLargestError,
            describe("the visibility follows the occlusion integral; largest error", largest));
}

// A rejected sample weighs exactly what an unoccluding one does: replacing the
// floor below the table's edge (beyond the radius, rejected) with more flat
// table top (in the tangent plane, no occlusion) changes no visibility on the
// original top. Normalizing by the samples that survive darkens the edge.
void testRejectedSamplesCountAsUnoccluded() {
    float largest = 0.0f;
    long compared = 0;
    for (const glm::vec3& position : kTableViewpoints) {
        const Eye eye{lookFrom(position, kTableTarget), desktopProjection()};
        const DepthImage edged = render(tableScene(), eye);
        const DepthImage extended = render(tableScene(3.0f), eye);
        const SsaoInputs inputs = inputsFor(eye, edged);
        for (int y = 0; y < edged.height; ++y) {
            for (int x = 0; x < edged.width; ++x) {
                const size_t i = edged.index(x, y);
                if (edged.object[i] != kTable || edged.worldNormal[i].y < 0.5f ||
                    edged.worldPoint[i].x > kTableEdge - 0.02f)
                    continue;
                // A texel too thin to confirm its face has its normal, and
                // so its visibility, decided by its neighbours.
                if (!faceSpansThreeTexels(edged, x, y)) continue;
                largest = std::max(largest, std::abs(rawVisibilityAt(edged, inputs, x, y) -
                                                     rawVisibilityAt(extended, inputs, x, y)));
                ++compared;
            }
        }
    }
    require(compared > 2000, "the views cover the table top");
    require(largest < 1e-4f, describe("samples rejected past the table's edge count as unoccluded; "
                                      "largest change", largest));
}

bool projectToPixel(const Eye& eye, const DepthImage& image, glm::vec3 world, int& x, int& y) {
    const glm::vec4 clip = eye.projection * eye.view * glm::vec4(world, 1.0f);
    if (clip.w <= 0.0f) return false;
    const glm::vec2 uv = glm::vec2(clip) / clip.w * 0.5f + 0.5f;
    x = int(std::floor(uv.x * float(image.width)));
    y = int(std::floor(uv.y * float(image.height)));
    return x >= 0 && y >= 0 && x < image.width && y < image.height;
}

// The visibility the image shows at a world point, interpolated between the
// four nearest pixel centres as a reprojection would read it: it moves
// continuously with the view unless the per-pixel values themselves jump.
float visibilitySeenAt(const Eye& eye, const DepthImage& image, const SsaoInputs& inputs, glm::vec3 world) {
    const glm::vec4 clip = eye.projection * eye.view * glm::vec4(world, 1.0f);
    const glm::vec2 pixel = (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * glm::vec2(image.width, image.height) - 0.5f;
    const glm::ivec2 corner(glm::floor(pixel));
    const glm::vec2 f = pixel - glm::vec2(corner);
    require(corner.x >= 0 && corner.y >= 0 && corner.x + 1 < image.width && corner.y + 1 < image.height,
            "the tracked point stays in view");
    const float top = glm::mix(visibilityAt(image, inputs, corner.x, corner.y),
                               visibilityAt(image, inputs, corner.x + 1, corner.y), f.x);
    const float bottom = glm::mix(visibilityAt(image, inputs, corner.x, corner.y + 1),
                                  visibilityAt(image, inputs, corner.x + 1, corner.y + 1), f.x);
    return glm::mix(top, bottom, f.y);
}

void testCameraMotionChangesVisibilityGradually() {
    const std::vector<Box> scene = tableScene();
    const std::vector<glm::vec3> tracked{
        {0.05f, 0.75f, 0.0f}, {0.2f, 0.75f, 0.15f}, {0.35f, 0.75f, -0.15f}, {0.55f, 0.75f, 0.35f},
        {0.45f, 0.75f, 0.05f}};
    const glm::vec3 target(0.4f, 0.8f, 0.0f);
    float largestStep = 0.0f;
    float lowest = 1.0f;
    for (const glm::vec3& start : {glm::vec3(1.6f, 1.6f, 0.8f), glm::vec3(0.9f, 1.9f, 1.5f)}) {
        const glm::vec3 right = glm::normalize(glm::cross(target - start, glm::vec3(0.0f, 1.0f, 0.0f)));
        std::vector<float> previous(tracked.size(), -1.0f);
        for (int frame = 0; frame < 120; ++frame) {
            // 1 mm per frame: a slow head drift, a tenth of a pixel here.
            const glm::vec3 position = start + right * (0.001f * float(frame));
            const Eye eye{lookFrom(position, target), desktopProjection()};
            DepthImage image = render(scene, eye);
            const SsaoInputs inputs = inputsFor(eye, image);
            runKernel(image, inputs);
            for (size_t i = 0; i < tracked.size(); ++i) {
                const float visibility = visibilitySeenAt(eye, image, inputs, tracked[i]);
                lowest = std::min(lowest, visibility);
                if (previous[i] >= 0.0f) largestStep = std::max(largestStep, std::abs(visibility - previous[i]));
                previous[i] = visibility;
            }
        }
    }
    std::cout << "[ssao] camera motion: largest step " << largestStep << ", lowest " << lowest << '\n';
    require(lowest < 0.9f, describe("the tracked points sit in real occlusion", lowest));
    // A third of the share one sample carries (1 / 16 at intensity 1): no
    // sample may switch fully on or off under a 1 mm move.
    constexpr float kLargestStep = 0.02f;
    require(largestStep < kLargestStep, describe("a 1 mm camera step changes visibility by little; "
                                                 "largest step", largestStep));
}

void testBothEyesAgree() {
    const std::vector<Box> scene = tableScene();
    double totalDifference = 0.0;
    float largest = 0.0f;
    long compared = 0;
    for (const glm::vec3& head : {glm::vec3(1.4f, 1.6f, 1.2f), glm::vec3(-0.6f, 1.7f, 1.6f)}) {
        const glm::vec3 target(0.2f, 0.75f, 0.0f);
        const glm::vec3 right = glm::normalize(glm::cross(target - head, glm::vec3(0.0f, 1.0f, 0.0f)));
        const glm::vec3 forward = glm::normalize(target - head);
        const Eye left{lookFrom(head - right * 0.032f, head - right * 0.032f + forward), kLeftEye};
        const Eye rightEye{lookFrom(head + right * 0.032f, head + right * 0.032f + forward), kRightEye};
        DepthImage leftImage = render(scene, left);
        DepthImage rightImage = render(scene, rightEye);
        const SsaoInputs leftInputs = inputsFor(left, leftImage);
        const SsaoInputs rightInputs = inputsFor(rightEye, rightImage);
        runKernel(leftImage, leftInputs);
        runKernel(rightImage, rightInputs);
        for (int y = 0; y < leftImage.height; ++y) {
            for (int x = 0; x < leftImage.width; ++x) {
                const size_t i = leftImage.index(x, y);
                if (leftImage.object[i] == kNoObject) continue;
                int rx = 0, ry = 0;
                if (!projectToPixel(rightEye, rightImage, leftImage.worldPoint[i], rx, ry)) continue;
                const size_t j = rightImage.index(rx, ry);
                // The same surface point, seen by both eyes.
                if (rightImage.object[j] != leftImage.object[i] ||
                    glm::distance(rightImage.worldPoint[j], leftImage.worldPoint[i]) > 0.01f)
                    continue;
                const float difference = std::abs(visibilityAt(leftImage, leftInputs, x, y) -
                                                  visibilityAt(rightImage, rightInputs, rx, ry));
                totalDifference += difference;
                largest = std::max(largest, difference);
                ++compared;
            }
        }
    }
    const double mean = totalDifference / double(std::max(compared, 1L));
    std::cout << "[ssao] eyes: " << compared << " points, mean difference " << mean << ", largest " << largest << '\n';
    require(compared > 5000, "both eyes see much of the scene");
    // The eyes' kernels land on slightly different texels of the same
    // surfaces; what remains must stay a fraction of a percent on average.
    constexpr double kMeanDifference = 0.002;
    require(mean < kMeanDifference, describe("both eyes give a surface point the same visibility; "
                                             "mean difference", mean));
}
}  // namespace

namespace {
// The AO pass's reads: the nearest texel, clamped to the edge.
size_t sampledTexel(glm::vec2 viewportUV) {
    const int x = std::clamp(int(std::floor(viewportUV.x * float(sampled->width))), 0, sampled->width - 1);
    const int y = std::clamp(int(std::floor(viewportUV.y * float(sampled->height))), 0, sampled->height - 1);
    return sampled->index(x, y);
}
}  // namespace

float ssao_shader::sampleDepth(glm::vec2 viewportUV) {
    return sampled->depth[sampledTexel(viewportUV)];
}

float ssao_shader::sampleRawVisibility(glm::vec2 viewportUV) {
    return sampled->raw[sampledTexel(viewportUV)];
}

int main() {
    testDegenerateTangentsFallBackToTheViewer();
    testFlatFloorNeverOccludesItself();
    testSameSurfaceSideFromDepth();
    testNormalsStayOnTheirSurface();
    testSilhouettesNeitherDarkenNorHalo();
    testRejectedSamplesCountAsUnoccluded();
    testOcclusionFollowsTheIntegral();
    testCameraMotionChangesVisibilityGradually();
    testBothEyesAgree();
    std::cout << "[ssao] " << checks << " checks passed without a window\n";
}
