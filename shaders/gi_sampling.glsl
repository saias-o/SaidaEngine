#ifndef SAIDA_GI_SAMPLING_GLSL
#define SAIDA_GI_SAMPLING_GLSL
#include "web_compat.glsl"
#include "lighting_data.glsl"
#include "ddgi_common.glsl"
DECL_TEX2D(0,4,9,giIrradiance);
DECL_TEX2D(0,5,10,giVisibility);
// Cascades share an atlas; toroidal addresses retain probes when the grid moves.
ivec3 giWrap(ivec3 c,ivec3 size) { return c-ivec3(floor(vec3(c)/vec3(size)))*size; }
vec3 giVolumeUVW(vec3 wp) {
    return mat3(lights.giWorldToGrid)*(wp-lights.giCascadeOrigins[0].xyz)/(lights.giSpacing.xyz*vec3(lights.giCounts.xyz));
}
int giProbeIndex(ivec3 c) { return c.x+lights.giCounts.x*(c.y+lights.giCounts.y*c.z); }
vec2 giProbeUV(int probe,vec3 dir,int texels,vec2 size) {
    ivec2 tile=ivec2(probe%lights.giCounts.w,probe/lights.giCounts.w);
    return (vec2(tile*(texels+2))+1.0+(octEncode(dir)*.5+.5)*float(texels))/size;
}
// One tetrahedron vertex: x = irradiance weight, rgb in `sum`. Called four
// times with constant vertices so the eight fetches issue together.
void giProbeTap(int k,ivec3 c,vec3 g,vec3 N,float trilinear,bool visibilityTest,vec2 irrInTile,vec2 irrSize,vec2 visSize,
                inout vec3 sum,inout float weights,inout float valid) {
    c=clamp(c,ivec3(0),lights.giCounts.xyz-1);
    int probe=k*lights.giCounts.x*lights.giCounts.y*lights.giCounts.z+giProbeIndex(giWrap(c+lights.giCascadeWrap[k].xyz,lights.giCounts.xyz));
    ivec2 tile=ivec2(probe%lights.giCounts.w,probe/lights.giCounts.w);
    vec3 delta=transpose(mat3(lights.giWorldToGrid))*(vec3(c)-g);
    float distance=length(delta);vec3 direction=distance>1e-5?delta/distance:N;
    vec4 irradiance=textureLod(TEX2D(giIrradiance),(vec2(tile*(lights.giAtlas.x+2))+irrInTile)/irrSize,0.0);
    vec2 visibility=visibilityTest?textureLod(TEX2D(giVisibility),(vec2(tile*(lights.giAtlas.y+2))+1.0+(octEncode(-direction)*.5+.5)*float(lights.giAtlas.y))/visSize,0.0).rg:vec2(1e9);
    valid+=trilinear*irradiance.a;
    float probability=1.0;
    if(distance>visibility.x) {
        float variance=max(visibility.y-visibility.x*visibility.x,.0001);
        float d=distance-visibility.x;probability=variance/(variance+d*d);probability*=probability;
    }
    float facing=max(0.0,dot(direction,N)*.5+.5);facing*=facing;
    float w=trilinear*facing*probability*irradiance.a;
    sum+=irradiance.rgb*w;weights+=w;
}
vec4 sampleGICascade(int k,vec3 wp,vec3 N,vec3 V,bool visibilityTest) {
    float spacing=lights.giCascadeOrigins[k].w;
    vec3 biased=wp+(N*.12+V*.08)*spacing;
    vec3 g=mat3(lights.giWorldToGrid)*(biased-lights.giCascadeOrigins[k].xyz)/spacing;
    ivec3 base=ivec3(floor(g));vec3 f=fract(g);
    vec3 sum=vec3(0);float weights=0,valid=0;
    vec2 irrSize=vec2(textureSize(TEX2D(giIrradiance),0)),visSize=vec2(textureSize(TEX2D(giVisibility),0));
    // Tetrahedral interpolation without dynamic indexing (which NVIDIA spills to
    // local memory): its vertices are 0, the largest fraction's axis, all but
    // the smallest's, and 1.
    float fmax=max(f.x,max(f.y,f.z)),fmin=min(f.x,min(f.y,f.z)),fmid=f.x+f.y+f.z-fmax-fmin;
    ivec3 emax=f.x>=f.y && f.x>=f.z?ivec3(1,0,0):f.y>=f.z?ivec3(0,1,0):ivec3(0,0,1);
    ivec3 emin=f.z<=f.y && f.z<=f.x?ivec3(0,0,1):f.y<=f.x?ivec3(0,1,0):ivec3(1,0,0);
    vec2 irrInTile=1.0+(octEncode(N)*.5+.5)*float(lights.giAtlas.x);
    giProbeTap(k,base,g,N,1.0-fmax,visibilityTest,irrInTile,irrSize,visSize,sum,weights,valid);
    giProbeTap(k,base+emax,g,N,fmax-fmid,visibilityTest,irrInTile,irrSize,visSize,sum,weights,valid);
    giProbeTap(k,base+ivec3(1)-emin,g,N,fmid-fmin,visibilityTest,irrInTile,irrSize,visSize,sum,weights,valid);
    giProbeTap(k,base+ivec3(1),g,N,fmin,visibilityTest,irrInTile,irrSize,visSize,sum,weights,valid);
    // Invalid probes leave the normalisation; only a cell whose probes are
    // nearly all invalid hands its share to the coarser cascade.
    return vec4(sum/max(weights,.005),clamp(valid*4.0,0.0,1.0));
}
vec4 sampleGI(vec3 wp,vec3 N,vec3 V) {
    if(lights.giOrigin.w<.5) return vec4(0);
    // Solids filter at every distance: the nearest-probe shortcut plants use
    // reads probes buried in buildings, which leak sun or paint facades black.
    // Visibility moments matter where walls are thin against the probe spacing;
    // past giClipmap.w metres the back-face classification is enough.
    vec3 toEye=wp-lights.cameraPos.xyz;
    bool visibilityTest=dot(toEye,toEye)<float(lights.giClipmap.w*lights.giClipmap.w);
    // One cascade per point, the finest holding it; only its edge band also
    // reads the next one. Every probe fetch is a cache miss in the atlas.
    int count=lights.giClipmap.x,k=0;float fade=0.0;
    for(;k<count;++k) {
        vec3 grid=mat3(lights.giWorldToGrid)*(wp-lights.giCascadeOrigins[k].xyz)/lights.giCascadeOrigins[k].w;
        vec3 edge=min(grid,vec3(lights.giCounts.xyz-1)-grid);
        fade=min(edge.x,min(edge.y,edge.z))*(count>1?.5:2.0);
        if(fade>0.0) break;
    }
    if(k==count) return vec4(0);
    vec4 value=sampleGICascade(k,wp,N,V,visibilityTest);
    // Two cells let incoming probes refine before taking full weight. The
    // next cascade also answers wherever this
    // one's probes are not traced yet (entering, invalidated): light then
    // refines from the coarse cascade instead of popping from the flat sky.
    float share=min(fade,1.0)*value.a;
    if(share<.999) {
        vec4 coarse=k+1<count?sampleGICascade(k+1,wp,N,V,visibilityTest):vec4(0);
        float coarseShare=(1.0-share)*coarse.a;
        float total=share+coarseShare;
        value=vec4((value.rgb*share+coarse.rgb*coarseShare)/max(total,1e-4),total);
    }
    return value;
}
// Outdoor plants use one directional sample per vertex. Their own leaf/ground
// occlusion stays in their shading; visibility-filtered sampling is for solids.
vec4 samplePlantGI(vec3 wp,vec3 N) {
    if(lights.giOrigin.w<.5) return vec4(0);
    vec3 sum=vec3(0);float coverage=0;
    vec2 atlas=vec2(textureSize(TEX2D(giIrradiance),0));
    for(int k=0;k<lights.giClipmap.x;++k) {
        vec3 g=mat3(lights.giWorldToGrid)*(wp-lights.giCascadeOrigins[k].xyz)/lights.giCascadeOrigins[k].w;
        vec3 edge=min(g,vec3(lights.giCounts.xyz-1)-g);
        float share=clamp(min(edge.x,min(edge.y,edge.z))*(lights.giClipmap.x>1?.5:1.0),0.0,1.0);
        if(share<=0.0) continue;
        ivec3 c=clamp(ivec3(floor(g+vec3(.5))),ivec3(0),lights.giCounts.xyz-1);
        int probe=k*lights.giCounts.x*lights.giCounts.y*lights.giCounts.z+giProbeIndex(giWrap(c+lights.giCascadeWrap[k].xyz,lights.giCounts.xyz));
        vec4 value=textureLod(TEX2D(giIrradiance),giProbeUV(probe,N,lights.giAtlas.x,atlas),0.0);
        share*=(1.0-coverage)*value.a;sum+=value.rgb*share;coverage+=share;
        if(coverage>.999) break;
    }
    return vec4(sum/max(coverage,1e-6),coverage);
}

#endif
