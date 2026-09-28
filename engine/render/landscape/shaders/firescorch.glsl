// The fire field's mask (FireScorchMap): one texel per 2 m cell of the
// fire window, R = scorch (0 untouched .. 1 burnt), G = ember glow
// (fireGlow: flames, then the cooling embers); cell (col, row) is
// centered on origin + (col, row) * texel. uFireScorchInfo = {originX,
// originZ, 1/texel, n} — n = 0 while nothing burnt.
// Burnt ground (docs/FIRE-RENDER.md F1) is a BED OF COALS: the front is
// eroded by the tileable Perlin-Worley volume (NoiseVolume, unit 12),
// the char is split into coal bodies by its Worley cracks, the cracks
// glow with the cell's ember level and breathe, the cooled coals turn to
// ash. Without the volume (no compute caps) a hash lattice stands in.
#include "firemask.glsl"
layout(binding = 12) uniform sampler3D uFireNoise;

float fireHash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// The volume's channels at world xz: r = Perlin-Worley (smooth, the
// front's erosion and the coals' phase), b = high-frequency Worley FBM
// (1 on a coal's body, falling to 0 in the cracks).
vec4 fireNoiseAt(vec2 xz) {
    if (uFireEmberInfo.w < 0.5) {
        float h = fireHash(floor(xz * 2.0));
        return vec4(h, h, fireHash(floor(xz * 3.0) + 0.5), 1.0);
    }
    return textureLod(uFireNoise, vec3(xz / max(uFireEmberHot.w, 0.5),
                                    uTime.x * 0.004), 0.0);
}

// charred: 0..1 the ground is char (the eroded scorch); glow: 0..1 the
// cell's ember level — its flames while it burns, then the short
// cooling behind the front (emberSeconds), then nothing.
void fireFront(vec2 xz, out float charred, out float glow) {
    vec3 m = fireMaskAt(xz);
    glow = m.y;
    charred = 0.0;
    if (m.x <= 0.0) {
        return;
    }
    float n = fireNoiseAt(xz).r;
    float e = m.x + (n - 0.5) * uFireEmberInfo.z;
    charred = smoothstep(0.42, 0.56, e);
}

// The coal bed's extent: the ground blackens the moment its cell burns
// (the bed lies UNDER the flames), and stays char once consumed.
float fireBurn(float charred, float glow) {
    return max(charred, smoothstep(0.15, 0.6, glow));
}

// The charred ground's colour: coal bodies dark, turning to ash as the
// embers die; the cracks stay dark (their light is fireEmber's).
vec3 fireCharAlbedo(vec3 albedo, float charred, float glow, vec2 xz) {
    float burn = fireBurn(charred, glow);
    if (burn <= 0.0) {
        return albedo;
    }
    vec4 nv = fireNoiseAt(xz);
    float body = smoothstep(0.35, 0.7, nv.b);
    vec3 ashColor = uFireCharInfo.rgb * 4.0 + vec3(0.12);
    // Coals show only where the embers still glow; behind, plain char.
    float ashed = uFireEmberCold.w * body * glow;
    vec3 coal = mix(uFireCharInfo.rgb, ashColor, ashed);
    return mix(albedo, coal, burn);
}

// The coal bed's light (HDR: the bloom bites on it): the cracks between
// the coals, by the cell's ember level, breathing each at its own phase.
vec3 fireEmber(float charred, float glow, vec2 xz) {
    float burn = fireBurn(charred, glow);
    if (uFireEmberInfo.x <= 0.0 || glow <= 0.0 || burn <= 0.0) {
        return vec3(0.0);
    }
    vec4 nv = fireNoiseAt(xz);
    float crack = 1.0 - smoothstep(0.3, 0.62, nv.b);
    vec3 ramp = mix(uFireEmberCold.rgb, uFireEmberHot.rgb, glow);
    float breath = 1.0 + uFireEmberInfo.y *
                             sin(uTime.x * 2.5 + nv.r * 25.0 + nv.g * 9.0);
    // Fresh cells (glow near 1) burn as a whole; cooled ones only in the
    // cracks.
    float bed = crack + glow * glow * 0.5;
    return ramp * uFireEmberInfo.x * breath * glow * glow * bed * burn;
}
