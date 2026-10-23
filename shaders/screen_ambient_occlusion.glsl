#ifndef SCREEN_AMBIENT_OCCLUSION_GLSL
#define SCREEN_AMBIENT_OCCLUSION_GLSL

// The screen-space ambient occlusion of the surface being shaded, for the
// fragment shaders of geometry the depth pre-pass draws. AmbientOcclusionPass
// computes it (ssao.glsl) between that pre-pass and the scene pass: one texel
// per pixel, one layer per view, its alpha the occlusion and, when GI runs, its
// colour the diffuse irradiance of the GI stage. Geometry the pre-pass leaves out must not read
// it, since a pixel's value then belongs to the surface behind. It occludes
// indirect light only (lighting.glsl `indirectOcclusion`), never direct light.
// Include after lighting.glsl; outside WEB the includer enables
// GL_EXT_multiview, whose gl_ViewIndex is 0 outside a multiview pass.

DECL_TEX2DARRAY(0, 17, 18, screenAmbientOcclusionMap);

vec4 screenIndirectTexel() {
#ifdef WEB
    int layer = 0;
#else
    int layer = gl_ViewIndex;
#endif
    return texelFetch(TEX2DARRAY(screenAmbientOcclusionMap),
                      ivec3(ivec2(gl_FragCoord.xy), layer), 0);
}

// The map's alpha: AO visibility (1 unoccluded).
float screenAmbientOcclusion() {
    if (lights.ambientOcclusion.x < 0.5) return 1.0;
    return screenIndirectTexel().a;
}

// The map's colour, when lights.ambientOcclusion.y says the GI stage ran: the
// diffuse irradiance lighting.glsl's giIrradianceOrSky gives this surface.
vec3 screenIndirectIrradiance() {
    return screenIndirectTexel().rgb;
}

#endif  // SCREEN_AMBIENT_OCCLUSION_GLSL
