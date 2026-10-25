// Keeps a shared GLSL source despite bindings and push constants absent from WGSL.

#ifndef WEB_COMPAT_GLSL
#define WEB_COMPAT_GLSL

// `_set` avoids replacing the keyword `set` in layout(set = ...).

#ifdef WEB
// Explicit LOD keeps shadow reads valid outside uniform control flow.
#extension GL_EXT_texture_shadow_lod : require

#define DECL_TEX2D(_set, tb, sb, name) \
    layout(set = _set, binding = tb) uniform texture2D name##_t; \
    layout(set = _set, binding = sb) uniform sampler   name##_s
#define TEX2D(name) sampler2D(name##_t, name##_s)

#define DECL_TEX2DARRAY(_set, tb, sb, name) \
    layout(set = _set, binding = tb) uniform texture2DArray name##_t; \
    layout(set = _set, binding = sb) uniform sampler        name##_s
#define TEX2DARRAY(name) sampler2DArray(name##_t, name##_s)

#define DECL_TEX3D(_set, tb, sb, name) \
    layout(set = _set, binding = tb) uniform texture3D name##_t; \
    layout(set = _set, binding = sb) uniform sampler   name##_s
#define TEX3D(name) sampler3D(name##_t, name##_s)

#define DECL_TEXCUBE(_set, tb, sb, name) \
    layout(set = _set, binding = tb) uniform textureCube name##_t; \
    layout(set = _set, binding = sb) uniform sampler     name##_s
#define TEXCUBE(name) samplerCube(name##_t, name##_s)

#define DECL_SHADOW2DARRAY(_set, tb, sb, name) \
    layout(set = _set, binding = tb) uniform texture2DArray name##_t; \
    layout(set = _set, binding = sb) uniform samplerShadow  name##_s
#define SHADOW2DARRAY(name) sampler2DArrayShadow(name##_t, name##_s)
#define SAMPLE_SHADOW2DARRAY(name, coord) \
    textureLod(SHADOW2DARRAY(name), coord, 0.0)

// WGSL emulates push constants with a UBO at group 3.
#define PUSH_QUALIFIER layout(set = 3, binding = 0) uniform

#else
#define DECL_TEX2D(_set, tb, sb, name)         layout(set = _set, binding = tb) uniform sampler2D name
#define TEX2D(name) name
#define DECL_TEX2DARRAY(_set, tb, sb, name)    layout(set = _set, binding = tb) uniform sampler2DArray name
#define TEX2DARRAY(name) name
#define DECL_TEX3D(_set, tb, sb, name)         layout(set = _set, binding = tb) uniform sampler3D name
#define TEX3D(name) name
#define DECL_TEXCUBE(_set, tb, sb, name)       layout(set = _set, binding = tb) uniform samplerCube name
#define TEXCUBE(name) name
#define DECL_SHADOW2DARRAY(_set, tb, sb, name) layout(set = _set, binding = tb) uniform sampler2DArrayShadow name
#define SHADOW2DARRAY(name) name
#define SAMPLE_SHADOW2DARRAY(name, coord) texture(name, coord)
#define PUSH_QUALIFIER layout(push_constant) uniform

#endif

#endif  // WEB_COMPAT_GLSL
