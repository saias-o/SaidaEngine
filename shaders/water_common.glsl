#include "water_types.glsl"

layout(set = 1, binding = 0) uniform WaterBlock {
    GpuWater items[WATER_MAX];
} waters;

PUSH_QUALIFIER WaterPush {
    uint index;   // which entry in waters.items this draw uses
    float time;   // animation clock (s)
} push;
