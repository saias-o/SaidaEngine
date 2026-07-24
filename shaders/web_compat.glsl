// Maintient une source GLSL commune malgré les bindings et push constants absents de WGSL.

#ifndef WEB_COMPAT_GLSL
#define WEB_COMPAT_GLSL

// `_set` évite de remplacer le mot-clé `set` dans layout(set = ...).

#ifdef WEB
// Le LOD explicite garde les lectures d'ombre valides hors flux de contrôle uniforme.
#extension GL_EXT_texture_shadow_lod : require

#define DECL_TEX2D(_set, tb, sb, name) \
    layout(set = _set, binding = tb) uniform texture2D name##_t; \
    layout(set = _set, binding = sb) uniform sampler   name##_s
#define TEX2D(name) sampler2D(name##_t, name##_s)

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

// WGSL émule les push constants par un UBO au groupe 3.
#define PUSH_QUALIFIER layout(set = 3, binding = 0) uniform

#else
#define DECL_TEX2D(_set, tb, sb, name)         layout(set = _set, binding = tb) uniform sampler2D name
#define TEX2D(name) name
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
