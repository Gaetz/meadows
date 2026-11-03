#version 460 core
#include "compat.glsl"
#include "common.glsl"
#include "sky.glsl"
#include "clouds.glsl"
#include "view_util.glsl"
#include "prop_encoding.glsl"

// Far-tree impostor. ATLAS look (uImpostorInfo.x): the tile of the
// tree's variant nearest the view direction — two azimuths blended,
// the elevation ring picked — read from TreeImpostorAtlas; the baked
// colour is the real tree's albedo, lit here with an ellipsoid normal
// over the quad (ambient + sun x cloud shadow), dissolved by applyFog.
// ANALYTIC look (the A/B): a few hash-jittered crown discs over a thin
// trunk (the lobe-tree family's read at distance), flat-lit.
layout(binding = 6) uniform sampler2D uTreeAtlas;

MEADOWS_PUSH_CONSTANTS(ImpostorPush) {
    vec4 uImpostorInfo;       // x atlas on, y azimuths, z rings, w ring step (rad)
    vec4 uImpostorVariant[5]; // x fit/height, y crown start/height
};

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec4 vParams;
layout(location = 3) in float vFade;
layout(location = 4) in vec3 vToCam;
layout(location = 5) in float vHandoff;
layout(location = 0) out vec4 fragColor;

const float kRows = 5.0;        // atlas rows = tree variants
const float kBakedAlpha = 0.745; // tree.frag's neutral relief alpha

float hash1(float n) {
    return fract(sin(n) * 43758.5453123);
}

// Per-instance hue roll, relative to the neutral roll the atlas was
// baked with (tree.frag's foliage roll at tint 0.5).
vec3 hueRoll(float jitter) {
    return mix(vec3(0.85, 1.0, 0.75), vec3(1.1, 1.0, 1.15), jitter) /
           vec3(0.975, 1.0, 0.95);
}

void main() {
    // Dither dissolve for the distance fades (IGN — keeps depth writes).
    // vHandoff is the real tree's presence: it keeps the pixels where
    // its presence beats the same noise (tree.frag), we keep the rest.
    float ign = ignJitter(gl_FragCoord.xy);
    if (vFade <= ign || vHandoff > ign) {
        discard;
    }
    float cloudVis = cloudShadowFactor(vWorldPos);
    float jitter = fract(vParams.y);

    if (uImpostorInfo.x > 0.5) {
        int variant = int(clamp(floor(vParams.y), 0.0, kRows - 1.0));
        float azimuths = uImpostorInfo.y;
        // The view direction in the tree's frame: the seed yaws each
        // impostor so a wood never shows one silhouette in rank.
        float yaw = hash1(vParams.x * 0.731) * 6.2831853;
        float az = atan(vToCam.x, vToCam.z) + yaw;
        float azN = fract(az / 6.2831853) * azimuths;
        float a0 = floor(azN);
        float fa = azN - a0;
        float a1 = mod(a0 + 1.0, azimuths);
        float el = atan(vToCam.y, length(vToCam.xz));
        float ring = clamp(floor(el / uImpostorInfo.w + 0.5), 0.0,
                           uImpostorInfo.z - 1.0);
        vec2 tile = vec2(vUv.x + 0.5, vUv.y);
        vec2 scale = vec2(1.0 / (azimuths * uImpostorInfo.z), 1.0 / kRows);
        vec2 uv0 = (vec2(ring * azimuths + a0, float(variant)) + tile) * scale;
        vec2 uv1 = (vec2(ring * azimuths + a1, float(variant)) + tile) * scale;
        vec4 t = mix(texture(uTreeAtlas, uv0), texture(uTreeAtlas, uv1), fa);
        // Coverage rides the alpha (cleared 0, covered ~kBakedAlpha);
        // the colour is un-premultiplied by it.
        float cover = clamp(t.a / kBakedAlpha, 0.0, 1.0);
        if (cover < 0.4) {
            discard;
        }
        vec3 albedo = (t.rgb / max(cover, 0.4)) * hueRoll(jitter);
        // Ellipsoid normal over the quad — the crown reads lit toward
        // the sun and shaded away from it, like the near canopies.
        vec3 fwd = normalize(vToCam);
        vec3 right = normalize(vec3(fwd.z, 0.0, -fwd.x));
        vec3 up = vec3(0.0, 1.0, 0.0);
        float crown = uImpostorVariant[variant].y / max(uImpostorVariant[variant].x, 1.0);
        float mid = mix(crown, 1.0, 0.55);
        vec3 nl = normalize(vec3(vUv.x * 2.0, (vUv.y - mid) * 1.5, 0.8));
        vec3 n = normalize(right * nl.x + up * nl.y + fwd * nl.z);
        float ndl = clamp(dot(n, uSunDirection.xyz) * 0.8 + 0.2, 0.0, 1.0);
        vec3 lit = albedo * (uAmbientColor.rgb + uSunColor.rgb * (ndl * cloudVis));
        fragColor = vec4(applyFog(lit, vWorldPos, cloudVis), 1.0);
        return;
    }

    // Crown: discs jittered by the instance seed, filling the band
    // above the measured trunk fraction; trunk: a thin stem under
    // them. Everything in quad space (x -0.5..0.5, y 0..1).
    float s = vParams.x;
    float trunkFrac = vParams.w;
    float crownMid = trunkFrac + (1.0 - trunkFrac) * 0.55;
    float crownHalf = (1.0 - trunkFrac) * 0.55;
    float inside = 0.0;
    if (isConiferImpostor(vParams)) {
        // CONIFER (prop_encoding.glsl): a ragged cone from a low crown
        // base to the apex — the whorl shelves read as edge steps.
        float base = trunkFrac * 0.55;
        float t = clamp((vUv.y - base) / max(1.0 - base, 1e-3), 0.0, 1.0);
        float edge = 0.46 * (1.0 - t) *
                     (0.78 + 0.22 * hash1(s + floor(t * 6.0)));
        inside = step(abs(vUv.x), edge) * step(vUv.y, 1.0) *
                 step(base, vUv.y);
    } else {
        for (int i = 0; i < 4; ++i) {
            float fi = float(i);
            vec2 center =
                vec2((hash1(s + fi * 7.31) - 0.5) * 0.42,
                     crownMid + (hash1(s + fi * 3.77) - 0.5) * crownHalf);
            float radius =
                (0.26 + hash1(s + fi * 11.9) * 0.16) * (1.0 - trunkFrac) /
                0.6;
            vec2 q = vUv - center;
            q.y *= 1.1; // slightly flattened lobes
            inside = max(inside, step(length(q), radius));
        }
    }
    float trunk = step(abs(vUv.x), 0.03) *
                  step(vUv.y, trunkFrac + 0.08);
    if (max(inside, trunk) < 0.5) {
        discard;
    }
    vec3 base = mix(vec3(0.13, 0.22, 0.10), vec3(0.20, 0.30, 0.13),
                    jitter);
    vec3 tint = mix(vec3(0.23, 0.16, 0.11), base, inside); // trunk brown
    // Flat lighting: no normal on a silhouette — ambient + a slice of
    // sun that follows the cloud shadows so the far woods flicker with
    // the passing clouds like everything else.
    vec3 lit = tint * (uAmbientColor.rgb + uSunColor.rgb * (0.55 * cloudVis));
    fragColor = vec4(applyFog(lit, vWorldPos, cloudVis), 1.0);
}
