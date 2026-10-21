// Grid sampling shared by drawing and conservative cell culling.
float sampleAt(int base, int i, int j) {
    i = clamp(i, 0, RES);
    j = clamp(j, 0, RES);
    return heights[base * SAMPLES * SAMPLES + j * SAMPLES + i];
}

// Bilinear between samples, at fractional grid coordinates.
float heightAt(int base, vec2 g) {
    g = clamp(g, vec2(0.0), vec2(float(RES)));
    ivec2 i0 = min(ivec2(floor(g)), ivec2(RES - 1));
    vec2 f = g - vec2(i0);
    float a = sampleAt(base, i0.x, i0.y), b = sampleAt(base, i0.x + 1, i0.y);
    float c = sampleAt(base, i0.x, i0.y + 1), d = sampleAt(base, i0.x + 1, i0.y + 1);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

vec2 morphGrid(vec2 g) {
    float halfRes = 0.5 * float(RES);
    vec2 fromCentre = abs(g - vec2(halfRes)) / halfRes;
    float alpha = clamp((max(fromCentre.x, fromCentre.y) - RING_MORPH_START) /
                        (RING_MORPH_END - RING_MORPH_START), 0.0, 1.0);
    return g - mod(g, 2.0) * alpha;
}
