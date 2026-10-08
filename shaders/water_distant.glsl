// Runtime terrain water layers share the WaterNode defaults, with the layer's
// own body tint, roughness and wave scale. No terrain-level-dependent detail.
GpuWater distantWater(vec4 body, vec2 waves, vec4 dynamics) {
    GpuWater w;
    w.area = vec4(0);
    w.deep = body;
    w.foam = vec4(.80,.90,.96,1);
    w.waveA = vec4(waves,.7,.25);
    w.detail1 = vec4(.7,.06,.2,100);
    w.detail2 = vec4(.9,.12,.35,110);
    w.look = vec4(5,35,.25,.2);
    w.misc = vec4(.1,200,.06,6);
    w.shoreColor = vec4(.20,.58,.62,.6);
    w.shoreGeom = vec4(0);
    w.shoreTune = vec4(.5,1.2,.4,1.5);
    w.shoreMode = vec4(0);
    w.cartoonWave = vec4(0);
    w.cartoonDetail = vec4(0);
    w.cartoonLook = vec4(0);
    w.cartoonShore = vec4(0);
    w.dynamics = dynamics;
    w.optics = vec4(0);   // drawn in the opaque terrain pass: nothing beneath
    w.localToWorld = mat4(1);
    return w;
}

// Continuous material coverage on the same cell-centred stencil as terrain
// materials. Interpolate parameters only between water contributors.
void terrainWaterLayer(GpuTerrain t, int base, vec2 grid, out float weight,
                       out vec4 body, out vec2 waves, out vec4 dynamics) {
    vec2 q = clamp(grid-.5,vec2(0),vec2(float(RES-1)));
    ivec2 c = ivec2(floor(q));
    vec2 f = smoothstep(vec2(0),vec2(1),fract(q));
    weight = 0.0; body = vec4(0); waves = vec2(0); dynamics = vec4(0);
    for (int i=0; i<4; ++i) {
        ivec2 offset = ivec2(i & 1,i >> 1);
        uint id = layerOf(base,c.x+offset.x,c.y+offset.y);
        float a = (offset.x==0 ? 1.0-f.x : f.x) * (offset.y==0 ? 1.0-f.y : f.y) * t.macros[id].w;
        weight += a;
        body += a*t.layers[id];
        waves += a*t.textures[id].zw;
        dynamics += a*t.waterDynamics[id];
    }
    body /= max(weight,.0001);
    waves /= max(weight,.0001);
    dynamics /= max(weight,.0001);
}
