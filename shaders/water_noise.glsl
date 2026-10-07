// Compact gradient noise with analytic derivatives. The simplex support avoids
// the zero-slope horizontal/vertical lines of interpolated value noise.
#include "noise.glsl"

vec3 waterNoise(vec2 p) {
    const float F = 0.366025404, G = 0.211324865;
    vec2 cell = floor(p + (p.x + p.y) * F);
    vec2 a = p - cell + (cell.x + cell.y) * G;
    vec2 step1 = a.x > a.y ? vec2(1, 0) : vec2(0, 1);
    vec2 b = a - step1 + G;
    vec2 c = a - 1.0 + 2.0 * G;
    const vec2 gradients[8] = vec2[8](vec2(1,0),vec2(-1,0),vec2(0,1),vec2(0,-1),
        vec2(.70710678,.70710678),vec2(-.70710678,.70710678),
        vec2(.70710678,-.70710678),vec2(-.70710678,-.70710678));
    vec3 result = vec3(0);
    for (int i = 0; i < 3; ++i) {
        vec2 x = i == 0 ? a : (i == 1 ? b : c);
        vec2 corner = cell + (i == 0 ? vec2(0) : (i == 1 ? step1 : vec2(1)));
        vec2 g = gradients[min(int(hash21(ivec2(corner)) * 8.0), 7)];
        float h = max(.5 - dot(x, x), 0.0);
        float h3 = h * h * h, h4 = h3 * h;
        float v = dot(g, x);
        result += vec3(h4 * v, h4 * g - 8.0 * h3 * v * x);
    }
    return vec3(.5,0,0) + 49.0 * result;
}

const mat2 WATER_ROT = mat2(.8, -.6, .6, .8);

float waterFilteredNoise(vec2 p, float footprint) {
    float shown = 1.0-smoothstep(.12,.45,footprint);
    return shown > 0.0 ? mix(.5,waterNoise(p).x,shown) : .5;
}

// The Jacobian carries each octave's gradient back to world axes. Dropped
// slope energy goes to the BRDF rather than vanishing at a distance cutoff.
vec2 waterRipples(vec2 p, vec2 flow, float time, float footprint, out float lost) {
    vec2 slope = vec2(0);
    mat2 basis = mat2(1);
    float gain = .48, frequency = 1.0;
    lost = 0.0;
    for (int i = 0; i < 3; ++i) {
        float shown = 1.0 - smoothstep(.12, .45, footprint * frequency);
        if (shown > 0.0) {
            vec3 n = waterNoise(basis * (p + flow * time) * frequency + float(i) * 17.13);
            slope += transpose(basis) * n.yz * (gain * shown);
        }
        lost += (1.0 - shown * shown) * gain * gain * .75;
        basis = WATER_ROT * basis;
        frequency *= 2.03;
        gain *= .55;
    }
    return slope;
}
