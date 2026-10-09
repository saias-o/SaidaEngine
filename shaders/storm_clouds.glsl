#ifndef SAIDA_STORM_CLOUDS_GLSL
#define SAIDA_STORM_CLOUDS_GLSL

// An angular cloud layer over the authored HDR sky. Fixed work, no textures
// or ray march: broad rain bases, rising cumulus lobes and spreading anvils.
// The direction is world-space, so the same cloud is stable in both XR eyes.
uint stormHash(uvec3 p) {
    uint h = p.x * 1597334677u ^ p.y * 3812015801u ^ p.z * 2798796415u;
    h ^= h >> 16u; h *= 2246822519u; h ^= h >> 13u;
    return h;
}

float stormNoise(vec3 p) {
    ivec3 cell = ivec3(floor(p));
    vec3 f = fract(p); f = f * f * (3.0 - 2.0 * f);
    const float inv = 1.0 / 4294967295.0;
    float a = mix(float(stormHash(uvec3(cell))), float(stormHash(uvec3(cell + ivec3(1,0,0)))), f.x);
    float b = mix(float(stormHash(uvec3(cell + ivec3(0,1,0)))), float(stormHash(uvec3(cell + ivec3(1,1,0)))), f.x);
    float c = mix(float(stormHash(uvec3(cell + ivec3(0,0,1)))), float(stormHash(uvec3(cell + ivec3(1,0,1)))), f.x);
    float d = mix(float(stormHash(uvec3(cell + ivec3(0,1,1)))), float(stormHash(uvec3(cell + ivec3(1,1,1)))), f.x);
    return mix(mix(a,b,f.y), mix(c,d,f.y), f.z) * inv;
}

float stormDetail(vec3 p) {
    return 0.57 * stormNoise(p) + 0.29 * stormNoise(p * 2.07 + vec3(11.7,3.1,8.5))
         + 0.14 * stormNoise(p * 4.13 + vec3(2.3,19.1,6.7));
}

float stormUnion(float a, float b, float width) {
    float h = max(width - abs(a - b), 0.0) / width;
    return min(a,b) - h * h * width * 0.25;
}

float stormEllipse(vec2 p, vec2 center, vec2 radius) {
    return length((p - center) / radius) - 1.0;
}

// RGB is cloud radiance; alpha is the fraction covering the HDR background.
// Night illumination comes from the actual sky, never from a daytime floor.
vec4 stormCloudLayer(vec3 dir, vec3 sunDirection, float cover, vec3 skyRadiance) {
    if (cover <= 0.0 || dir.y < -0.04) return vec4(0.0);
    float detail = stormDetail(dir * vec3(13.0,10.0,13.0) + vec3(4.8,13.1,7.3));
    float billow = stormNoise(dir * vec3(6.0,9.0,6.0) + vec3(18.1,3.7,5.2));
    const vec2 axes[4] = vec2[4](vec2(0.96,0.28), vec2(-0.23,0.97),
                               vec2(-0.88,-0.48), vec2(0.35,-0.94));
    const float towerHeights[4] = float[4](0.56,0.43,0.61,0.49);
    float mass = 2.0;
    for (int tower = 0; tower < 4; ++tower) {
        vec2 axis = axes[tower];
        float front = dot(dir.xz,axis);
        vec2 p = vec2(dot(dir.xz,vec2(-axis.y,axis.x)),dir.y);
        float height = towerHeights[tower];
        float base = stormEllipse(p,vec2(0.0,0.105),vec2(0.53,0.105));
        float trunk = stormEllipse(p,vec2(-0.035,height * 0.48),vec2(0.23,height * 0.44));
        float crown = stormEllipse(p,vec2(0.025,height * 0.80),vec2(0.31,height * 0.30));
        float anvil = stormEllipse(p,vec2(0.10,height),vec2(0.63,0.085));
        float cloud = stormUnion(stormUnion(base,trunk,0.36),stormUnion(crown,anvil,0.28),0.32);
        // Back-facing masses do not reflect through to the opposite horizon.
        cloud += (1.0 - smoothstep(0.10,0.55,front)) * 5.0;
        mass = min(mass,cloud);
    }
    mass += (detail - 0.5) * 0.48 + (billow - 0.5) * 0.30;
    float towers = 1.0 - smoothstep(-0.18,0.14,mass);
    // As rain thickens, the upper cloud ceiling closes above the towers.
    float ceiling = smoothstep(0.20,0.72,dir.y + (billow - 0.5) * 0.20);
    ceiling *= smoothstep(0.40,0.92,cover) * mix(0.70,1.0,detail);
    float density = max(towers * cover,ceiling);
    density *= smoothstep(-0.025,0.04,dir.y);
    float opacity = 1.0 - exp(-4.8 * density);

    float edgeLight = smoothstep(-0.65,0.02,mass);
    float sunFacing = max(dot(dir,sunDirection),0.0);
    float lining = pow(sunFacing,6.0) * edgeLight;
    float underside = smoothstep(0.045,0.32,dir.y);
    float shade = 0.16 + 0.15 * underside + 0.18 * detail + 0.27 * lining;
    float luminance = dot(max(skyRadiance,vec3(0.0)),vec3(0.2126,0.7152,0.0722));
    vec3 neutral = vec3(luminance);
    vec3 light = mix(neutral,max(skyRadiance,vec3(0.0)),0.12);
    vec3 cloudColor = light * shade * vec3(0.94,0.97,1.02);
    return vec4(cloudColor,opacity);
}

#endif
