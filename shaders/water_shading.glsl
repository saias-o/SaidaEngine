// Shared by WaterNode and distant water layers. No descriptors or derivatives
// here: callers supply world-space position and a footprint computed uniformly.
#include "water_wave.glsl"

float waterFootprint(vec3 p, vec3 up) {
    vec3 ray = p - lights.cameraPos.xyz;
    vec3 rd = normalize(ray);
    return length(ray) * max(length(dFdx(rd)), length(dFdy(rd))) / max(abs(dot(rd, up)), .035);
}

vec2 waterDirection(float degrees) {
    float a = radians(degrees);
    return vec2(cos(a), sin(a));
}

vec4 shadeWater(vec3 p, vec3 up, GpuWater w, float time, float footprint) {
    vec3 V = normalize(lights.cameraPos.xyz - p);
    float depth = waterDepthAt(p.xz, w);
    int mode = int(w.shoreMode.x + .5);
    float shallow = mode == 0 ? 1.0 : smoothstep(0.0, max(w.shoreTune.w,.01), depth);
    vec2 domain; mat2 jacobian; vec3 group;
    waterWaveDomain(p.xz, w, time, domain, jacobian, group);
    group = mix(vec3(1,0,0),group,waterTrainShown(6.2831853/max(w.waveA.y*3.7,1.0),footprint));
    vec2 waveSlope = vec2(0);
    float variance = 0.0, height = 0.0, amplitude = 0.0;
    vec2 windDir = waveDirAt(p.xz, w);
    for (int i = 0; i < WATER_TRAINS; ++i) {
        WaterTrain tr = waterTrain(i, w, windDir);
        float shown = waterTrainShown(tr.wn, footprint);
        float slope = tr.amp * tr.wn * shallow;
        if (shown > 0.0) {
            float phase = dot(tr.dir, domain) * tr.wn - time * tr.celerity * tr.wn + tr.phase;
            waveSlope += transpose(jacobian) * tr.dir * (slope * cos(phase) * shown);
            height += tr.amp * sin(phase) * shown;
        }
        amplitude += tr.amp;
        variance += (1.0 - shown * shown) * .5 * slope * slope;
    }
    waveSlope = waveSlope*group.x + height*shallow*group.yz;
    variance *= group.x*group.x;
    float crest = amplitude > .0001 ? clamp(.5 + .5 * height / amplitude, 0.0, 1.0) : 0.0;

    // Broad moving wind streaks, with a second scale for capillary patches.
    // Frequencies are in metres and independent of the mesh / terrain level.
    vec2 windUV = vec2(dot(p.xz, windDir), dot(p.xz, vec2(-windDir.y, windDir.x)));
    float gust = .65 * waterFilteredNoise(windUV * vec2(.0018,.006) + vec2(time * .003,13.2),footprint*.006)
               + .35 * waterFilteredNoise(WATER_ROT * p.xz * .021 + vec2(5.3,time * .009),footprint*.021);
    float wind = mix(1.0,mix(.45, 1.25, smoothstep(.22,.78,gust)),w.dynamics.w);
    float lost1, lost2;
    vec2 g1 = waterRipples(p.xz * w.detail1.x, waterDirection(w.detail1.w) * w.detail1.y,
                          time, footprint * w.detail1.x, lost1) * w.detail1.z;
    vec2 g2 = waterRipples(p.xz * w.detail2.x, waterDirection(w.detail2.w) * w.detail2.y,
                          time, footprint * w.detail2.x, lost2) * w.detail2.z;
    // Preserve the author's distance control smoothly, retaining slope energy.
    float distanceGain = mix(.65,1.0,exp(-length(p-lights.cameraPos.xyz)/max(w.misc.y,1.0)));
    int profile = int(w.dynamics.x+.5);
    float rippleGain = wind * distanceGain * shallow * w.dynamics.y * (profile == 0 ? .4 : (profile == 2 ? 1.2 : 1.0));
    vec2 slope = waveSlope + (g1 + g2) * rippleGain;
    variance += (lost1*w.detail1.z*w.detail1.z + lost2*w.detail2.z*w.detail2.z)*rippleGain*rippleGain;
    vec3 tangent = normalize(vec3(up.y,-up.x,0));
    vec3 bitangent = cross(tangent,up);
    vec3 N = normalize(up - tangent * slope.x - bitangent * slope.y);
    // Fold invisible back-facing microfacets toward the viewer continuously.
    N = normalize(N + V * max(0.0,.06-dot(N,V)));
    float NoV = max(dot(N,V),.001);
    float baseRough = clamp(w.deep.w,.025,.8);
    // GGX's alpha is perceptual roughness squared, hence its variance is r^4.
    float rough = clamp(pow(pow(baseRough,4.0) + .5 * variance, .25), .025,.65);
    bool hasEnv = lights.environmentParams.x > .5;
    // The sky light the body scatters back, as every lit material receives it
    // (accumulate: the scene ambient plus the diffuse environment). A scene
    // that turns diffuse IBL off still lights its water with its ambient.
    vec3 ambient = lights.ambient.rgb;
    if (hasEnv) ambient += environmentIrradiance(up) * lights.environmentParams.y;
    vec3 L = up;
    vec3 sun = vec3(0);
    float visibility = 1.0;
    for (int i=0; i<lights.counts.x; ++i) {
        if (int(lights.lights[i].dirType.w + .5) == 0) {
            Light light = lights.lights[i];
            L = normalize(-light.dirType.xyz);
            sun = light.colorInt.rgb * light.colorInt.w;
            if (light.spotShadow.z >= 0.0)
                visibility = shadowFactor(int(light.spotShadow.z+.5), p, max(dot(N,L),0.0));
            if (i == int(lights.sunOcclusion.x)) visibility *= sunOcclusionAt(p);
            break;
        }
    }
    float sunUp = max(dot(L,up),0.0);
    sun *= visibility * smoothstep(-.02,.02,dot(L,up));
    // Beer-Lambert attenuation of the shallow tint, with faster red absorption.
    vec3 transmission = exp(-max(depth,0.0) * vec3(1.8,.8,.5) / max(w.misc.w,.01));
    vec3 tint = mode == 0 ? w.deep.rgb : mix(w.deep.rgb,w.shoreColor.rgb,transmission);
    vec3 body = tint * (ambient * .8 + sun * sunUp * .16);
    float forward = pow(max(dot(-V,L),0.0),4.0);
    body += w.shoreColor.rgb * sun * (forward * smoothstep(.55,.95,crest) * .10 * shallow);

    vec3 R = reflect(-V,N);
    // The available environment is the sky hemisphere; soften the horizon
    // crossing rather than making all downward rays share a clamped stripe.
    R = normalize(R + up * (abs(dot(R,up)) - dot(R,up)));
    vec3 reflected = hasEnv ? sampleEnvironmentLod(R,rough * environmentMaxLod()) * lights.environmentParams.z
                            : mix(lights.ambient.rgb, vec3(.17,.35,.60) * max(dot(ambient,vec3(.333)),0.0),
                                  sqrt(max(dot(R,up),0.0)));
    float fresnel = .02037 + .97963 * pow(1.0-NoV,max(w.look.x,1.0));
    float reflectionWeight = clamp(fresnel * w.foam.w,0.0,1.0);
    // What Fresnel does not reflect enters the water. Of it, `transparency`
    // comes back from what lies beneath (the framebuffer under this pixel,
    // through alpha) and the rest is the body's own scattered tint. A shore
    // knows its depth, and its deeper water lets less through.
    float through = w.optics.x;
    if (mode != 0) through *= dot(transmission,vec3(1.0/3.0));
    float beneath = (1.0-reflectionWeight) * through;
    vec3 color = body * (1.0-reflectionWeight) * (1.0-through) + reflected * reflectionWeight;

    vec3 H = normalize(L + V + up * .00001);
    float NoL = max(dot(N,L),0.0);
    // A finite source and unresolved normal variance bound the sun highlight.
    // specularPower remains an artistic tightness control (35 = neutral).
    float sunRough = max(rough * pow(35.0/max(w.look.y,1.0),.125),.075);
    float D = distributionGGX(max(dot(N,H),0.0),sunRough);
    float G = geometrySmith(NoV,NoL,sunRough);
    float F = .02037 + .97963 * pow(1.0-max(dot(H,V),0.0),5.0);
    color += sun * min(D*G*F/max(4.0*NoV,.001),12.0) * (w.look.z*4.0);

    // Sparse breaking crests. Never interpolate a foam mask from vertices:
    // that exposes every triangle when displacement is filtered out.
    float foam = 0.0;
    if ((w.misc.z > 0.0 && footprint < 2.0) || (mode != 0 && w.shoreMode.y > 0.0)) {
        float resolved = 1.0-smoothstep(.3,2.0,footprint);
        float lace = waterFilteredNoise(WATER_ROT * (p.xz * .8 + windDir * time * .18),footprint*.8);
        float breaker = smoothstep(.58+.24*w.look.w,.94,crest);
        foam = breaker * smoothstep(.38,.72,lace) * w.misc.z * 3.0 * wind * resolved * min(w.dynamics.y,1.5);
        if (mode != 0) {
            float along = dot(p.xz,vec2(-windDir.y,windDir.x));
            float wash = .5+.5*sin(time*w.shoreTune.y + along*.045 + lace*.6);
            float band = max(w.shoreTune.x + w.shoreTune.z*wash,.001);
            float wet = 1.0-smoothstep(.15*band,band,depth);
            float front = 1.0-smoothstep(.015,.09+footprint*.1,abs(depth-band));
            float shoreFoam = max(wet*.55,front) * mix(.45,1.0,lace) * w.shoreMode.y;
            foam = max(foam,shoreFoam);
        }
    }
    vec3 foamLit = w.foam.rgb * (ambient*.65 + sun*sunUp*.3);
    foam = clamp(foam,0.0,1.0);
    color = mix(color,foamLit,foam);
    // Foam is opaque. `color` is premultiplied by the surface's coverage;
    // un-premultiply it for the straight alpha blend.
    float coverage = 1.0 - beneath * (1.0-foam);
    float alpha = mode == 0 ? 1.0 : smoothstep(0.0,max(w.shoreColor.w,.01),depth);
    return vec4(color / max(coverage,.05), alpha * coverage);
}
