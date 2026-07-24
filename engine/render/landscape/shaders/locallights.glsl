// Local lights:
// the 16 nearest LightSource placements, filled per frame by the scene
// (flicker already applied). Point + spot, NO shadows (the key-light
// shadow is separate, below). The landscape (terrain/grass) stays sun-only
// by decision — only meshes and characters include this.

// The ONE shadowed interior key light (matched by position below).
layout(binding = 6) uniform sampler2DShadow uKeyShadow;

layout(std140, binding = 5) uniform LightsUbo {
    vec4 uLightCount;                 // x = active lights
    vec4 uLightPositionRadius[24];    // xyz world, w radius (m)
    vec4 uLightColorIntensity[24];    // rgb premultiplied color*intensity
    // APPEND-only UBO (new members at the END, both sides):
    // xyz = spot direction (normalized), w = cos(half angle); w = -2
    // marks a point light, w = -3 a WINDOW projector (xyz = the
    // window's into-room normal — docs/LIGHTING.md).
    vec4 uLightDirectionAngle[24];
    // Window projector half extents (xy).
    vec4 uLightWindowInfo[24];
};

// The window-projector clip (docs/LIGHTING.md §3): the beam is the
// window's rectangle extruded along the live sun. Projects `p` back
// along the beam onto the window plane; inside the frame = lit, with a
// soft 6 cm edge. Also carries the facing gate (sun on the window's
// outside) and kills points BEHIND the window plane.
float windowBeam(vec3 p, vec3 anchor, vec3 nrm, vec2 halfExtents) {
    vec3 beam = -uSunDirection.xyz;
    float denom = dot(beam, nrm);
    float gate = smoothstep(0.15, 0.40, denom);
    if (gate <= 0.001) {
        return 0.0;
    }
    float s = dot(p - anchor, nrm) / max(denom, 0.15);
    if (s < 0.0) {
        return 0.0; // outside the wall
    }
    vec3 local = (p - beam * s) - anchor;
    vec3 upw = abs(nrm.y) > 0.95 ? vec3(1.0, 0.0, 0.0)
                                 : vec3(0.0, 1.0, 0.0);
    vec3 rightw = normalize(cross(upw, nrm));
    vec3 realUp = cross(nrm, rightw);
    float u = dot(local, rightw);
    float v = dot(local, realUp);
    const float kEdge = 0.06;
    return gate *
           smoothstep(0.0, kEdge, halfExtents.x - abs(u)) *
           smoothstep(0.0, kEdge, halfExtents.y - abs(v));
}

vec3 localLights(vec3 worldPos, vec3 n) {
    vec3 sum = vec3(0.0);
    int count = int(uLightCount.x + 0.5);
    // Interior pass (uCascadeSplits.w = 1 inside interior cells; the
    // exterior look stays byte-identical at 0): pure N·L leaves every
    // surface facing away from a candle pitch flat — dead rooms. Indoors
    // the diffuse wraps (half-Lambert) and each light adds a small
    // normal-free bounce so the room takes its hue, a cheap stand-in for
    // the first GI bounce.
    float interior = clamp(uCascadeSplits.w, 0.0, 1.0);
    for (int i = 0; i < count; ++i) {
        vec3 toLight = uLightPositionRadius[i].xyz - worldPos;
        float dist = length(toLight);
        float radius = uLightPositionRadius[i].w;
        if (dist >= radius) {
            continue;
        }
        // Falloff (hot-tunable selector): 1 = WINDOWED INVERSE-SQUARE
        // (UE-style — physical concentration near the source, light dies
        // quickly with distance; picked after the quintic
        // "lit too far"), 0 = the stylized quintic ramp.
        const int kFalloff = 1;
        float atten;
        if (kFalloff == 1) {
            float window =
                clamp(1.0 - pow(dist / radius, 4.0), 0.0, 1.0);
            atten = (window * window) / (dist * dist + 1.0);
        } else {
            float x = 1.0 - dist / radius;
            atten = x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
        }
        vec3 l = toLight / max(dist, 1e-3);
        // Spot cone: w = cos(half angle), soft 10%-of-cone edge.
        // w = -3: window projector — the frame clips instead of a cone.
        float cosHalf = uLightDirectionAngle[i].w;
        if (cosHalf < -2.5) {
            atten *= windowBeam(worldPos, uLightPositionRadius[i].xyz,
                                uLightDirectionAngle[i].xyz,
                                uLightWindowInfo[i].xy);
            l = -(-uSunDirection.xyz); // light the surface FROM the sun
        } else if (cosHalf > -1.5) {
            float cd = dot(-l, uLightDirectionAngle[i].xyz);
            float edge = mix(cosHalf, 1.0, 0.1);
            atten *= smoothstep(cosHalf, edge, cd);
        }
        // The key light's shadow map (one per interior) stops this
        // light from bleeding through walls. Matched by position.
        if (uKeyShadowInfo.w > 0.5 &&
            distance(uLightPositionRadius[i].xyz, uKeyShadowInfo.xyz) <
                0.05) {
            vec4 lc = uKeyShadowViewProj * vec4(worldPos, 1.0);
            if (lc.w > 0.0) {
                vec3 proj = lc.xyz / lc.w;
                proj.xy = proj.xy * 0.5 + 0.5; // 0..1: xy only
                if (proj.z < 1.0 &&
                    all(greaterThan(proj.xy, vec2(0.0))) &&
                    all(lessThan(proj.xy, vec2(1.0)))) {
                    atten *= texture(
                        uKeyShadow, vec3(proj.xy, proj.z - 0.0022));
                }
            }
        }
        float ndl = dot(n, l);
        float wrapped = clamp((ndl + 0.4) / 1.4, 0.0, 1.0);
        float diff = mix(max(ndl, 0.0), wrapped, interior);
        // The normal-free bounce is the OMNI candles' room hue; on a
        // spot it paints the cone's cross-section on the walls it
        // crosses (the window-circle artifact). Beams bounce via GI.
        float bounce = 0.18 * atten * interior *
                       (cosHalf > -1.5 ? 0.0 : 1.0);
        sum += uLightColorIntensity[i].rgb * (diff * atten + bounce);
    }
    return sum;
}
