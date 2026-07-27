// Local lights:
// the scene's selected LightSource placements (frustum + importance,
// game/SceneSubmit.cpp), flicker already applied. Point + spot, NO
// shadows (the key-light shadow is separate, below). The landscape
// (terrain/grass) stays sun-only until the clustered path gates its
// cost (docs/RENDERING.md §5 B4).

#include "clusters.glsl"

// The ONE shadowed interior key light (matched by position below).
layout(binding = 6) uniform sampler2DShadow uKeyShadow;

// The per-cluster light lists (cluster_cull.comp; docs/RENDERING.md §5).
// Unbound while the clustered path is off — never read then.
layout(std430, binding = 4) readonly buffer ClusterLights {
    uint uClusterLights[];
};

layout(std140, binding = 5) uniform LightsUbo {
    vec4 uLightCount;                 // x = active lights
    vec4 uLightPositionRadius[64];    // xyz world, w radius (m)
    vec4 uLightColorIntensity[64];    // rgb premultiplied color*intensity
    // APPEND-only UBO (new members at the END, both sides):
    // xyz = spot direction (normalized), w = cos(half angle); w = -2
    // marks a point light, w = -3 a WINDOW projector (xyz = the
    // window's into-room normal — docs/RENDERING.md).
    vec4 uLightDirectionAngle[64];
    // Window projector half extents (xy); z = key-shadow atlas slot + 1
    // (0 = unshadowed — docs/RENDERING.md §5 B6).
    vec4 uLightWindowInfo[64];
};

// The light's key-shadow atlas tile (2x2 of the shared target). The
// bounds stay one texel inside the tile so the PCF taps never cross
// into a neighbour.
float keyShadowFactor(int i, vec3 p) {
    int slot = int(uLightWindowInfo[i].z + 0.5) - 1;
    if (slot < 0) {
        return 1.0;
    }
    vec4 lc = uKeyShadowAtlas[slot] * vec4(p, 1.0);
    if (lc.w <= 0.0) {
        return 1.0;
    }
    vec3 proj = lc.xyz / lc.w;
    proj.xy = proj.xy * 0.5 + 0.5;
    if (proj.z >= 1.0 || any(lessThan(proj.xy, vec2(0.002))) ||
        any(greaterThan(proj.xy, vec2(0.998)))) {
        return 1.0;
    }
    vec2 atlasUv =
        (proj.xy + vec2(slot & 1, slot >> 1)) * 0.5;
    return texture(uKeyShadow, vec3(atlasUv, proj.z - 0.0022));
}

// The window-projector clip (docs/RENDERING.md §3): the beam is the
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

// One light's full contribution — shared by the legacy loop and the
// clustered path (identical math, only the iteration set differs).
vec3 shadeLocalLight(int i, vec3 worldPos, vec3 n, float interior) {
    {
        vec3 toLight = uLightPositionRadius[i].xyz - worldPos;
        float dist = length(toLight);
        float radius = uLightPositionRadius[i].w;
        if (dist >= radius) {
            return vec3(0.0);
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
        // The light's key shadow (atlas tile) stops it from bleeding
        // through walls.
        atten *= keyShadowFactor(i, worldPos);
        float ndl = dot(n, l);
        float wrapped = clamp((ndl + 0.4) / 1.4, 0.0, 1.0);
        // Exterior local lights follow the SAME stylized cel ramp (and
        // A/B blend) as the sun — night characters under torchlight
        // step instead of grading through smooth Lambert. Interiors
        // keep the wrapped half-Lambert + bounce (the dead-room fix).
        float diff = mix(stylizedDiffuse(ndl, max(ndl, 0.0)), wrapped,
                         interior);
        // The normal-free bounce is the OMNI candles' room hue; on a
        // spot it paints the cone's cross-section on the walls it
        // crosses (the window-circle artifact). Beams bounce via GI.
        float bounce = 0.18 * atten * interior *
                       (cosHalf > -1.5 ? 0.0 : 1.0);
        return uLightColorIntensity[i].rgb * (diff * atten + bounce);
    }
}

vec3 localLights(vec3 worldPos, vec3 n) {
    vec3 sum = vec3(0.0);
    // Interior pass (uCascadeSplits.w = 1 inside interior cells; the
    // exterior look stays byte-identical at 0): pure N·L leaves every
    // surface facing away from a candle pitch flat — dead rooms. Indoors
    // the diffuse wraps (half-Lambert) and each light adds a small
    // normal-free bounce so the room takes its hue, a cheap stand-in for
    // the first GI bounce.
    float interior = clamp(uCascadeSplits.w, 0.0, 1.0);
    if (uClusterInfo.x > 0.5) {
        // Clustered path (docs/RENDERING.md §5): only this pixel's cell.
        // The z slice uses CAMERA DISTANCE — the froxel grid's metric.
        vec2 uv = gl_FragCoord.xy * uScreenInfo.zw;
        ivec2 cellXy = clamp(ivec2(uv * vec2(kClusterDims.xy)), ivec2(0),
                             kClusterDims.xy - 1);
        float far = max(uClusterInfo.y, kSliceNear + 1.0);
        float dist = distance(worldPos, uCameraPos.xyz);
        int cellZ =
            clamp(int(sliceCoord(dist, float(kClusterDims.z), far)), 0,
                  kClusterDims.z - 1);
        int base = clusterIndex(ivec3(cellXy, cellZ)) * kClusterSlots;
        int listed = int(uClusterLights[base]);
        for (int s = 0; s < listed; ++s) {
            sum += shadeLocalLight(int(uClusterLights[base + 1 + s]),
                                   worldPos, n, interior);
        }
    } else {
        // Legacy loop, capped at the pre-clustered budget: passes whose
        // frame data leaves the clustered flag off (reflection) degrade
        // to the 24 NEAREST (the list is distance-ordered).
        int count = min(int(uLightCount.x + 0.5), 24);
        for (int i = 0; i < count; ++i) {
            sum += shadeLocalLight(i, worldPos, n, interior);
        }
    }
    return sum;
}
