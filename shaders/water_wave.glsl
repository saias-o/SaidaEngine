// Shared ocean and shaped-water displacement; only the input surface differs.
void waterWaveAt(vec2 xz, float baseY, GpuWater w, float t,
                 out vec3 pos, out vec3 normal, out float crest) {
    float wavelength = max(w.waveA.y, 0.01);
    float speed = w.waveA.z;
    float chop = w.waveA.w;
    float depth = waterDepthAt(xz, w);
    float shallow = (int(w.shoreMode.x + 0.5) == 0)
        ? 1.0
        : smoothstep(0.0, max(w.shoreTune.w, 0.01), depth);
    int waveMode = int(w.shoreMode.x + 0.5);
    vec2 baseDir = waveDirAt(xz, w);
    const float FAN[5] = float[5](0.0, 13.0, -10.0, 18.0, -6.0);
    float h = 0.0, dhdx = 0.0, dhdz = 0.0;
    vec2 disp = vec2(0.0);
    float a = w.waveA.x;
    float wn = 6.2831853 / wavelength;
    for (int i = 0; i < 5; ++i) {
        float offDeg = (waveMode == 0) ? 36.87 * float(i) : FAN[i];
        vec2 dir = rotate2(baseDir, radians(offDeg));
        float ang = dot(dir, xz) * wn + t * speed * wn;
        float s = sin(ang), c = cos(ang);
        h    += a * s;
        dhdx += a * wn * dir.x * c;
        dhdz += a * wn * dir.y * c;
        disp -= dir * (a * chop * c);
        a *= 0.62;
        wn *= 1.87;
    }
    h *= shallow; dhdx *= shallow; dhdz *= shallow; disp *= shallow;
    vec2 xzC = xz + disp;
    pos = vec3(xzC.x, baseY + h, xzC.y);
    normal = normalize(vec3(-dhdx, 1.0, -dhdz));
    crest = clamp(h / max(w.waveA.x, 0.001) * 0.5 + 0.5, 0.0, 1.0);
}
