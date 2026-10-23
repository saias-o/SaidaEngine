#include "gi_voxel_data.glsl"
layout(set=0,binding=0,rgba8) uniform writeonly image3D voxelAlbedo;
layout(set=0,binding=2,rgba8) uniform writeonly image3D voxelNormal;
layout(set=0,binding=3,rgba16f) uniform writeonly image3D voxelEmission;
layout(std430,set=0,binding=4) buffer BrickJobs { uint jobs[]; };
