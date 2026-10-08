// Shared by grass.vert and grass.frag: the fields of GrassNodes in view
// (render/features/GrassFeature.hpp mirrors these layouts).

const int MAX_FIELDS = 9;     // GrassFeature::kMaxFields
const int MAX_GROUND = 65;    // GrassNode::kMaxGroundSamples
const int MAX_BOUNDARY = 257; // GrassNode::kMaxBoundarySamples
const int GROUND_STRIDE = MAX_GROUND * MAX_GROUND + 4 * MAX_BOUNDARY;
const int MAX_COVER = 512;    // GrassNode::kMaxCoverSize
const int MAX_BENDERS = 4;    // GrassNode::kMaxBenders
const int RING_CELLS = 192;   // GrassFeature::kRingCells: blades a ring side

struct GpuGrass {
    mat4 localToWorld;
    vec4 uvFromLocalU;  // u = dot(xyz, (x, z, 1))
    vec4 uvFromLocalV;
    vec4 blades;        // density a square metre, height, width at the root, radius
    vec4 wind;          // xy direction (node-local x, z, unit), z sway (m), w time (s)
    vec4 camera;        // xyz the camera, node-local
    ivec4 sizes;        // x ground samples, y cover texels, z/w south/north knots
    vec4 tuft;          // x blades a tuft, y how hard gusts lay the grass down (0 to 1)
    vec4 materialU;     // the ground material's s = dot(xyz, (x, z, 1))
    vec4 materialV;     // and t
    vec4 variation;     // the ground material's MaterialDesc::variation
    vec4 benders[MAX_BENDERS];  // node-local centre, radius
};

layout(set = 1, binding = 0) uniform GrassUBO {
    GpuGrass items[MAX_FIELDS];
} grass;

// Regular heights then two boundaries of MAX_BOUNDARY (u, y) knots a field.
layout(std430, set = 1, binding = 1) readonly buffer GroundBuffer {
    float ground[];
};

// MAX_COVER^2 RGBA8 texels a field.
layout(std430, set = 1, binding = 2) readonly buffer CoverBuffer {
    uint cover[];
};

layout(push_constant) uniform Push {
    vec2 origin;      // node-local x, z of cell (0, 0)'s corner of this ring
    float spacing;    // metres between blades in this ring
    uint slot;
    ivec2 cellMin;    // the first cell drawn
    uint cellsX;      // cells drawn a row
    uint ring;        // 0 nearest
    vec2 hole;        // half-extent of the ring inside (m), centred like this one; 0 none
    uint vertices;    // a blade's vertices
    uint pad;
} push;
