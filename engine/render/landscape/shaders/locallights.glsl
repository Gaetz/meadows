// Local lights (chantier 2 B5, spots + stylized falloff chantier 6 B1):
// the 16 nearest LightSource placements, filled per frame by the scene
// (flicker already applied). Point + spot, NO shadows (the key-light
// shadow is a later brick). The landscape (terrain/grass) stays sun-only
// by decision — only meshes and characters include this.

layout(std140, binding = 5) uniform LightsUbo {
    vec4 uLightCount;                 // x = active lights
    vec4 uLightPositionRadius[16];    // xyz world, w radius (m)
    vec4 uLightColorIntensity[16];    // rgb premultiplied color*intensity
    // B1 APPEND (the UBO lesson: new members at the END, both sides):
    // xyz = spot direction (normalized), w = cos(half angle); w = -2
    // marks a point light.
    vec4 uLightDirectionAngle[16];
};

vec3 localLights(vec3 worldPos, vec3 n) {
    vec3 sum = vec3(0.0);
    int count = int(uLightCount.x + 0.5);
    // B5 interior pass (uCascadeSplits.w = 1 inside interior cells; the
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
        // quickly with distance; dev pick 2026-07-07 after the quintic
        // "lit too far"), 0 = the B1 stylized quintic ramp.
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
        float ndl = dot(n, l);
        float wrapped = clamp((ndl + 0.4) / 1.4, 0.0, 1.0);
        float diff = mix(max(ndl, 0.0), wrapped, interior);
        float bounce = 0.18 * atten * interior;
        sum += uLightColorIntensity[i].rgb * (diff * atten + bounce);
    }
    return sum;
}
