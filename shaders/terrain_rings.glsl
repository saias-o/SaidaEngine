// Shared by terrain_rings.vert, terrain_rings.frag and terrain_rings_sun.comp:
// one TerrainRingsNode's rings (render/features/TerrainRingsFeature.hpp
// mirrors these layouts).

const int RES = 128;          // cells per level side: TerrainRingsNode::kResolution
const int SAMPLES = RES + 1;  // heights per level side
const int MAX_LEVELS = 12;    // TerrainRingsNode::kMaxLevels
const int MAX_LAYERS = 16;    // TerrainRingsNode::kMaxLayers
const int MAX_TERRAINS = 2;   // TerrainRingsFeature::kMaxTerrains
const int MAX_HOLES = 16;     // TerrainRingsNode::kMaxHoles
const float RING_MORPH_START = 0.8;
const float RING_MORPH_END = 0.95;

// Marches toward the light (terrain_rings_sun.comp, terrain_rings_occlusion.comp):
// at most this many steps, one cell of the level holding each point a step,
// and a penumbra of about a degree -- the Sun's disc and what a cell cannot
// place better -- as the tangent of its half-width.
const int MAX_SUN_STEPS = 512;
const float SUN_PENUMBRA = 0.018;

struct GpuTerrain {
    mat4 localToWorld;
    vec4 focus;                 // x, z (node-local), innerRadius, level count
    vec4 levels[MAX_LEVELS];    // originX, originZ, spacing, present (0/1)
    vec4 layers[MAX_LAYERS];    // rgb albedo, roughness
    vec4 textures[MAX_LAYERS];  // material slot + 1, UV scale, reserved
    vec4 macros[MAX_LAYERS];    // frequency, albedo variation, normal strength, reserved
    vec4 holes[MAX_HOLES * 2];  // per hole: (x0, z0, x1, z1), (x2, z2, x3, z3), node-local
    ivec4 holeCount;            // x
    vec4 sun;                   // xyz toward the shadowing light (node-local, unit); w its index + 1, 0 none
    vec4 relief;                // x the highest sample of any level
};

layout(set = 1, binding = 0) uniform TerrainUBO {
    GpuTerrain items[MAX_TERRAINS];
} terrains;

// Every level of every slot, SAMPLES^2 heights each.
layout(std430, set = 1, binding = 1) readonly buffer HeightBuffer {
    float heights[];
};

// One layer index a cell, four to a word.
layout(std430, set = 1, binding = 2) readonly buffer LayerBuffer {
    uint layerWords[];
};

// How much of the directional light reaches each sample, 0 to 1, laid out
// as the heights: written by terrain_rings_sun.comp, read by the rings.
#ifdef SUNLIGHT_WRITER
layout(std430, set = 1, binding = 3) writeonly buffer SunlightBuffer {
    float sunlight[];
};
#else
layout(std430, set = 1, binding = 3) readonly buffer SunlightBuffer {
    float sunlight[];
};
#endif

layout(push_constant) uniform Push {
    uint slot;
    uint level;
} push;

int levelBase(uint slot, uint level) {
    return int(slot * uint(MAX_LEVELS) + level);
}

uint layerOf(int base, int cx, int cz) {
    cx = clamp(cx, 0, RES - 1);
    cz = clamp(cz, 0, RES - 1);
    int cell = base * RES * RES + cz * RES + cx;
    return min((layerWords[cell >> 2] >> (uint(cell & 3) * 8u)) & 0xFFu, uint(MAX_LAYERS - 1));
}

// The finest level present that holds the node-local point p, -1 past them all.
int finestLevelHolding(GpuTerrain t, vec2 p) {
    int levels = int(t.focus.w);
    for (int l = 0; l < levels; ++l) {
        vec4 M = t.levels[l];
        if (M.w < 0.5) continue;
        vec2 g = (p - M.xy) / M.z;
        if (all(greaterThanEqual(g, vec2(0.0))) && all(lessThanEqual(g, vec2(float(RES))))) return l;
    }
    return -1;
}

// Level `level`'s height at node-local p, bilinear between its samples.
float ringHeight(GpuTerrain t, uint slot, int level, vec2 p) {
    vec4 M = t.levels[level];
    vec2 g = clamp((p - M.xy) / M.z, vec2(0.0), vec2(float(RES)));
    ivec2 i0 = min(ivec2(floor(g)), ivec2(RES - 1));
    vec2 f = g - vec2(i0);
    int row = levelBase(slot, uint(level)) * SAMPLES * SAMPLES + i0.y * SAMPLES + i0.x;
    float a = heights[row], b = heights[row + 1];
    float c = heights[row + SAMPLES], d = heights[row + SAMPLES + 1];
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
