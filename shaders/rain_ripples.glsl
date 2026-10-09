#ifndef SAIDA_RAIN_RIPPLES_GLSL
#define SAIDA_RAIN_RIPPLES_GLSL

// World-space precipitation: no eye, screen coordinate, mesh UV or texture.
// Callers supply gradients before discards/material divergence. The same
// event centres and ages are shared by mono, XR and Web shading.
const float RAIN_CELL_METRES = 0.70;
const float RAIN_WAVE_NUMBER = 58.0;
const float RAIN_EVENT_RATE = 0.82;
const float RAIN_MAX_FOOTPRINT = 0.085;

vec3 rainHash(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yzz) * p.zyx);
}

float rainNoise(vec2 p, float footprint) {
    vec2 at = floor(p), u = fract(p);
    u = u * u * (3.0 - 2.0 * u);
    float a = rainHash(vec3(at, 3.1)).x, b = rainHash(vec3(at + vec2(1, 0), 3.1)).x;
    float c = rainHash(vec3(at + vec2(0, 1), 3.1)).x, d = rainHash(vec3(at + vec2(1, 1), 3.1)).x;
    return mix(mix(mix(a, b, u.x), mix(c, d, u.x), u.y), 0.5, smoothstep(0.35, 1.0, footprint));
}

void rainPlane(out vec3 tangent, out vec3 bitangent) {
    vec3 up = lights.rainUp.xyz;
    tangent = normalize(cross(up, abs(up.y) < 0.9 ? vec3(0, 1, 0) : vec3(0, 0, 1)));
    bitangent = cross(tangent, up);
}

// Gaussian footprint filtering removes each wave before it is subpixel,
// including grazing views. Nine neighbouring cells keep expanding rings
// continuous over cell boundaries; event jitter changes only at a quiet birth.
vec2 rainImpactSlope(vec2 p, float seconds, float intensity, float footprint) {
    if (intensity <= 0.0 || footprint >= RAIN_MAX_FOOTPRINT) return vec2(0.0);
    float filtered = exp(-0.5 * RAIN_WAVE_NUMBER * RAIN_WAVE_NUMBER * footprint * footprint);
    vec2 cell = floor(p / RAIN_CELL_METRES);
    vec2 slope = vec2(0.0);
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        vec2 at = cell + vec2(x, y);
        float phase = seconds * RAIN_EVENT_RATE + rainHash(vec3(at, 7.3)).x;
        vec3 event = rainHash(vec3(at, floor(phase) + 13.7));
        float age = fract(phase);
        float present = smoothstep(event.z - 0.08, event.z + 0.08, intensity);
        float envelope = smoothstep(0.0, 0.045, age) * (1.0 - smoothstep(0.50, 0.95, age));
        vec2 centre = (at + 0.18 + 0.64 * event.xy) * RAIN_CELL_METRES;
        vec2 delta = p - centre;
        float distance = length(delta);
        float radius = 0.015 + age * 0.48;
        float band = (distance - radius) / 0.065;
        float wave = cos(RAIN_WAVE_NUMBER * (distance - radius)) * exp(-band * band);
        slope += delta / max(distance, 0.025) * wave * envelope * present;
    }
    return slope * filtered;
}

vec3 rainWorldSlope(vec3 p, vec3 geometricNormal, float footprint, float strength) {
    if (lights.rainParams.x <= 0.0 || strength <= 0.0) return vec3(0.0);
    vec3 tangent, bitangent;
    rainPlane(tangent, bitangent);
    vec2 plane = vec2(dot(p, tangent), dot(p, bitangent));
    vec2 gradient = rainImpactSlope(plane, lights.rainParams.z, lights.rainParams.x, footprint);
    vec3 slope = tangent * gradient.x + bitangent * gradient.y;
    return (slope - geometricNormal * dot(slope, geometricNormal)) * strength;
}

void applyRainSurface(vec3 p, vec3 geometricNormal, vec3 dx, vec3 dy, vec4 reception,
                      inout vec3 normal, inout vec3 albedo, inout float roughness,
                      inout vec3 dielectricF0, inout float dielectricF90) {
    if (reception.x <= 0.0 || max(lights.rainParams.x, lights.rainParams.y) <= 0.0) return;
    // The geometric face admits rainfall. A wall's normal map cannot turn it
    // into a puddle; down-facing undersides and vertical faces stay untouched.
    float facing = dot(geometricNormal, lights.rainUp.xyz);
    float slopeMask = smoothstep(0.60, 0.94, facing);
    if (slopeMask <= 0.0) return;
    float wet = lights.rainParams.y * reception.x * slopeMask;
    vec3 tangent, bitangent;
    rainPlane(tangent, bitangent);
    vec2 plane = vec2(dot(p, tangent), dot(p, bitangent));
    vec2 px = vec2(dot(dx, tangent), dot(dx, bitangent));
    vec2 py = vec2(dot(dy, tangent), dot(dy, bitangent));
    float footprint = max(length(px), length(py));
    float puddle = 0.0;
    if (wet > 0.0 && reception.y > 0.0) {
        float patches = 0.70 * rainNoise(plane * 0.32, footprint * 0.32)
                      + 0.30 * rainNoise(plane * 0.79 + 19.1, footprint * 0.79);
        puddle = wet * reception.y * smoothstep(0.57 - wet * 0.25, 0.79 - wet * 0.25, patches);
    }
    albedo *= 1.0 - wet * reception.w;
    roughness = mix(roughness, max(0.13, roughness * 0.52), wet);
    roughness = mix(roughness, 0.075, puddle);
    float coat = wet * (0.35 + 0.65 * puddle);
    dielectricF0 = mix(dielectricF0, vec3(0.02), coat);
    dielectricF90 = mix(dielectricF90, 1.0, coat);
    vec3 impacts = rainWorldSlope(p, geometricNormal, footprint,
                                  reception.z * reception.x * slopeMask * mix(0.22, 1.0, puddle));
    normal = normalize(mix(normal, geometricNormal, puddle * 0.75) - impacts);
}

#endif
