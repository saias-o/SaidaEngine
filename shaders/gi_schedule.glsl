struct Ray { vec4 dirDist;vec4 radiance; };
layout(std430,set=1,binding=0) buffer RaysBuffer { Ray rays[]; };
layout(std430,set=1,binding=5) buffer ScheduleBuffer { ivec4 schedule[]; };
DECL_TEX2D(1,3,10,previousIrradiance);
DECL_TEX2D(1,4,11,previousVisibility);
