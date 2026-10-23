layout(set=0,binding=1) uniform VoxelUBO {
    vec4 origin; vec4 extent; ivec4 res; ivec4 wrap;
    mat4 worldToGrid; mat4 axisVP[3];
} vox;
