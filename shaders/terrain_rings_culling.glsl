// Pixel-centre guard for conservative whole-cell rejection.
vec4 holeSides(vec4 ab, vec4 cd, vec2 p) {
    return vec4(edgeSide(ab.xy, ab.zw, p), edgeSide(ab.zw, cd.xy, p),
                edgeSide(cd.xy, cd.zw, p), edgeSide(cd.zw, ab.xy, p));
}

// Perspective interpolation makes side/w affine in screen space. A covered
// MSAA sample can shade at a pixel centre outside the triangle: keep a full
// pixel of that affine function on every edge, including at grazing angles.
bool rasterInsideTriangle(vec4 a, vec4 b, vec4 c, vec4 sidesA, vec4 sidesB, vec4 sidesC) {
    if (min(a.w, min(b.w, c.w)) <= 0.0) return false;
    vec2 u = b.xy / b.w - a.xy / a.w, v = c.xy / c.w - a.xy / a.w;
    float area = u.x * v.y - u.y * v.x;
    if (abs(area) < 1e-12) return false;
    vec4 sa = sidesA / a.w, sb = sidesB / b.w, sc = sidesC / c.w;
    vec4 dx = ((sb - sa) * v.y - (sc - sa) * u.y) / area;
    vec4 dy = ((sc - sa) * u.x - (sb - sa) * v.x) / area;
    vec4 guard = 2.0 * (abs(dx) * push.inverseWidth + abs(dy) * push.inverseHeight);
    return all(greaterThan(min(sa, min(sb, sc)), guard)) ||
           all(lessThan(max(sa, max(sb, sc)), -guard));
}

vec4 cellPoint(vec4 L, int base, vec2 grid) {
    vec2 g = morphGrid(grid);
    vec2 p = L.xy + g * L.z;
    return vec4(p.x, heightAt(base, g), p.y, 1.0);
}

bool rasterInsideCell(GpuTerrain t, vec4 L, int base, ivec2 cell, vec4 ab, vec4 cd, int vi) {
    if (push.inverseWidth <= 0.0 || push.inverseHeight <= 0.0) return false;
    mat4 toClip = t.localToClip[vi];
    vec2 grid = vec2(cell);
    vec4 p0 = cellPoint(L, base, grid), p1 = cellPoint(L, base, grid + vec2(1, 0));
    vec4 p2 = cellPoint(L, base, grid + vec2(1, 1)), p3 = cellPoint(L, base, grid + vec2(0, 1));
    vec4 a = toClip * p0, b = toClip * p1, c = toClip * p2, d = toClip * p3;
    vec4 sa = holeSides(ab, cd, p0.xz), sb = holeSides(ab, cd, p1.xz);
    vec4 sc = holeSides(ab, cd, p2.xz), sd = holeSides(ab, cd, p3.xz);
    return rasterInsideTriangle(a, b, c, sa, sb, sc) && rasterInsideTriangle(a, c, d, sa, sc, sd);
}
