#include "water_noise.glsl"

// Shared ocean and shaped-water waves; only the input surface differs.
//
// Five Gerstner trains. Their headings fan around the wind at irregular
// offsets and their wavelengths fall by an irrational ratio, so no two trains
// share a period and their sum never closes into a lattice. (Headings in
// even 37-degree steps with a 1.87 ratio did: seen from an aircraft, the sea
// was tiled.)
//
// A train is only drawn by the mesh when the mesh can carry it: shorter than
// eight vertex spacings, or than a few pixels, it can only facet or alias
// into a regular moire, the same tiles again. The fragment shader draws every train
// as normals instead, filtered by the pixel's footprint (water.frag).

const int WATER_TRAINS = 5;
// Each train is this much shorter than the last: the plastic number's
// inverse, irrational, so the periods never meet.
const float WATER_TRAIN_RATIO = 0.56984;

struct WaterTrain {
    vec2 dir;      // heading, unit
    float amp;     // metres
    float wn;      // wavenumber, radians per metre
    float phase;   // radians
    float celerity; // metres per second times the node's speed scale
};

WaterTrain waterTrain(int i, GpuWater w, vec2 baseDir) {
    int mode = int(w.shoreMode.x + 0.5);
    int profile = int(w.dynamics.x + .5);
    float spread = profile == 0 ? .28 : (profile == 2 ? 1.9 : 1.2217);
    float lengthScale = profile == 0 ? 1.8 : (profile == 2 ? .45 : 1.0);
    float heightScale = profile == 2 ? .65 : 1.0;
    const float FAN[5] = float[5](0.0, 13.0, -10.0, 18.0, -6.0);
    float offset = (mode == 0) ? (fract(0.5 + float(i) * 0.6180340) * 2.0 - 1.0) * spread
                               : radians(FAN[i]);
    WaterTrain tr;
    tr.dir = rotate2(baseDir, offset);
    float scale = pow(WATER_TRAIN_RATIO, float(i));
    // Equal steepness: a train's height is in proportion to its length.
    tr.amp = w.waveA.x * w.dynamics.y * scale * heightScale;
    if (profile == 0) tr.amp *= pow(.65,float(i));
    tr.wn = 6.2831853 / (max(w.waveA.y, 0.01) * scale * lengthScale);
    // Bound the sum of horizontal steepness below one: extreme authored
    // amplitude/intensity cannot fold the Gerstner mesh over itself.
    tr.amp = min(tr.amp,.75/(float(WATER_TRAINS)*tr.wn*max(w.waveA.w,.01)));
    tr.phase = float(i) * 2.399963;
    // Deep water disperses: a longer train runs faster (c ~ sqrt(lambda)).
    tr.celerity = w.waveA.z * sqrt(9.81 / tr.wn);
    return tr;
}

// How much of a train a surface sampled every `spacing` metres can show
// without its facets showing: all of it past eight samples a wavelength,
// none under four. A train drawn by too few vertices is a polyhedron, and
// seen grazing every facet shifts what the pixel sees by tens of centimetres:
// the ripples folded at every edge of the mesh.
float waterTrainShown(float wn, float spacing) {
    float samples = 6.2831853 / (wn * max(spacing, 1e-4));
    return smoothstep(4.0, 8.0, samples);
}

// Bend wavefronts continuously over several wavelengths. Both passes use the
// same domain and its Jacobian, so the silhouette and shading travel together.
void waterWaveDomain(vec2 p, GpuWater w, float time, out vec2 q, out mat2 jacobian, out vec3 group) {
    float scale = 1.0 / max(w.waveA.y * 3.7, 1.0);
    float amount = max(w.waveA.y, .01) * (.18 + .35 * w.misc.x);
    vec3 a = waterNoise(p * scale + vec2(time * w.waveA.z * .018, 7.1));
    vec3 b = waterNoise(p * scale + vec2(19.7, -time * w.waveA.z * .012));
    q = p + amount * (vec2(a.x, b.x) - .5);
    jacobian = mat2(1) + amount * scale * mat2(a.y, b.y, a.z, b.z);
    group = vec3(1.0 + w.dynamics.w * 1.6 * (a.x-.5), w.dynamics.w * 1.6 * scale * a.yz);
}

// The waves the mesh carries at `xz`. `spacing` is the coarser of the mesh's
// vertex spacing and a few of the pixels it covers there.
void waterWaveAt(vec2 xz, float baseY, GpuWater w, float t, float spacing,
                 out vec3 pos, out vec3 normal, out float crest) {
    float depth = waterDepthAt(xz, w);
    float shallow = (int(w.shoreMode.x + 0.5) == 0)
        ? 1.0
        : smoothstep(0.0, max(w.shoreTune.w, 0.01), depth);
    vec2 baseDir = waveDirAt(xz, w);
    float chop = w.waveA.w;
    vec2 domain; mat2 jacobian; vec3 group;
    waterWaveDomain(xz, w, t, domain, jacobian, group);
    float h = 0.0, dhdx = 0.0, dhdz = 0.0, full = 0.0;
    vec2 disp = vec2(0.0);
    for (int i = 0; i < WATER_TRAINS; ++i) {
        WaterTrain tr = waterTrain(i, w, baseDir);
        float ang = dot(tr.dir, domain) * tr.wn - t * tr.celerity * tr.wn + tr.phase;
        float a = tr.amp * waterTrainShown(tr.wn, spacing);
        float s = sin(ang), c = cos(ang);
        h    += a * s;
        vec2 gradient = transpose(jacobian) * tr.dir;
        dhdx += a * tr.wn * gradient.x * c;
        dhdz += a * tr.wn * gradient.y * c;
        disp -= tr.dir * (a * chop * c);
        full += tr.amp;
    }
    dhdx = (dhdx*group.x + h*group.y)*shallow;
    dhdz = (dhdz*group.x + h*group.z)*shallow;
    h *= shallow*group.x; disp *= shallow*group.x;
    vec2 xzC = xz + disp;
    pos = vec3(xzC.x, baseY + h, xzC.y);
    normal = normalize(vec3(-dhdx, 1.0, -dhdz));
    crest = clamp(h / max(full * 0.6, 0.001) * 0.5 + 0.5, 0.0, 1.0);
}

// The world position of the camera that `view` looks from.
vec3 waterCameraPosition(mat4 view) {
    return -(transpose(mat3(view)) * view[3].xyz);
}

// Metres one pixel covers at `distance`, using the actual render resolution.
float waterPixelFootprint(mat4 proj, float distance, float viewportHeight) {
    return distance * 2.0 / (max(abs(proj[1][1]), 1e-3) * max(viewportHeight,1.0));
}
