// Local lights:
// the 16 nearest LightSource placements, filled per frame by the scene
// (flicker already applied). Point + spot, NO shadows (the key-light
// shadow is separate, below). The landscape (terrain/grass) stays sun-only
// by decision — only meshes and characters include this.

// The ONE shadowed interior key light (matched by position below).
layout(binding = 6) uniform sampler2DShadow uKeyShadow;

layout(std140, binding = 5) uniform LightsUbo {
    vec4 uLightCount;                 // x = active lights
    vec4 uLightPositionRadius[16];    // xyz world, w radius (m)
    vec4 uLightColorIntensity[16];    // rgb premultiplied color*intensity
    // APPEND-only UBO (new members at the END, both sides):
    // xyz = spot direction (normalized), w = cos(half angle); w = -2
    // marks a point light.
    vec4 uLightDirectionAngle[16];
};

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
        float cosHalf = uLightDirectionAngle[i].w;
        if (cosHalf > -1.5) {
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
