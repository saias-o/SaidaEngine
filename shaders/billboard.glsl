// Spherical XY billboard. Atlas views are relative to the authored model's
// frame, so randomized instance yaw and rebased worlds retain their silhouette.
// Adjacent azimuth views blend; elevation takes the nearest baked row.
void faceBillboard(mat4 model, vec3 eye, vec2 atlas, inout vec3 position,
                   inout vec3 normal, inout vec3 tangent, inout vec2 uv,
                   out vec3 nextView) {
    nextView = vec3(0.0, 0.0, -1.0);
    if (atlas.x < 1.0) return;
    vec3 towardEye = (inverse(model) * vec4(eye, 1.0)).xyz;
    float radius = length(towardEye);
    vec3 facing = radius > 0.0001 ? towardEye / radius : vec3(0.0, 0.0, 1.0);
    vec3 right = vec3(facing.z, 0.0, -facing.x);
    right = length(right) > 0.0001 ? normalize(right) : vec3(1.0, 0.0, 0.0);
    vec3 up = normalize(cross(facing, right));
    mat3 frame = mat3(right, up, facing);
    position = frame * position;
    normal = frame * normal;
    tangent = frame * tangent;
    float azimuth = mod(atan(facing.x, facing.z) / 6.28318530718 + 1.0, 1.0) * atlas.x;
    float column = floor(azimuth);
    float row = atlas.y > 1.0 ? round((asin(clamp(facing.y, -1.0, 1.0)) / 3.14159265359 + 0.5) * (atlas.y - 1.0)) : 0.0;
    nextView = vec3((uv + vec2(mod(column + 1.0, atlas.x), row)) / atlas, fract(azimuth));
    uv = (uv + vec2(column, row)) / atlas;
}
