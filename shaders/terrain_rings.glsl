// Shared by terrain_rings.vert, terrain_rings.frag and terrain_rings_sun.comp:
// one TerrainRingsNode's rings (render/features/TerrainRingsFeature.hpp
// mirrors these layouts).

const int RES = 128;          // cells per level side: TerrainRingsNode::kResolution
const int SAMPLES = RES + 1;  // heights per level side
const int MAX_LEVELS = 12;    // TerrainRingsNode::kMaxLevels
const int MAX_LAYERS = 16;    // TerrainRingsNode::kMaxLayers
const int MAX_TERRAINS = 2;   // TerrainRingsFeature::kMaxTerrains
const int MAX_HOLES = 16;     // TerrainRingsNode::kMaxHoles

struct GpuTerrain {
    mat4 localToWorld;
    vec4 focus;                 // x, z (node-local), innerRadius, level count
    vec4 levels[MAX_LEVELS];    // originX, originZ, spacing, present (0/1)
    vec4 layers[MAX_LAYERS];    // rgb albedo, roughness
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
