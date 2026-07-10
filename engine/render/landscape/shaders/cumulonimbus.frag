#version 460 core
#include "common.glsl"
#include "sky.glsl"

// Brick 30 — cumuliform tower silhouette: FBM-eroded 2D mask, lit by the
// sky palette in two tones + a silver lining on the sun side. Faded in
// by uStormInfo.x (the crossfaded WeatherForm.stormFront) and out by the
// fog like everything distant.

in vec2 vUv;
in float vSeed;
in vec3 vWorldPos;
out vec4 fragColor;

float hash21(vec2 p) {
    p = fract(p * vec2(234.34, 435.345));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 s = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + vec2(1, 0)), s.x),
               mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), s.x),
               s.y);
}
float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; ++i) {
        v += vnoise(p) * a;
        p = p * 2.13 + vec2(17.3, 9.1);
        a *= 0.5;
    }
    return v;
}

void main() {
    float storm = uStormInfo.x;
    if (storm <= 0.003) {
        discard;
    }
    vec2 p = vec2(vUv.x * 2.2, vUv.y * 2.6) + vSeed * 37.0;
    // A tower: wide anvil base narrowing irregularly with height, eroded
    // by FBM; drift very slowly with the wind time.
    float drift = uWindInfo.x * 0.004;
    float body = fbm(p + vec2(drift, 0.0));
    float taper = 1.0 - vUv.y * (0.55 + 0.30 * vnoise(p * 0.7));
    float mask = body * taper - abs(vUv.x) * (0.55 - 0.25 * body);
    float alpha = smoothstep(0.18, 0.34, mask) * storm;
    if (alpha <= 0.004) {
        discard;
    }

    // Two-tone cel lighting from the sky palette + silver lining.
    float sunSide = clamp(
        dot(normalize(vec3(vUv.x, 0.0, 0.0) + vec3(0, vUv.y, 0)),
            normalize(vec3(uSunDirection.x, uSunDirection.y, 0.0))) *
                0.5 +
            0.5,
        0.0, 1.0);
    vec3 dark = mix(uHorizonFarColor.rgb, uZenithColor.rgb, 0.35) * 0.55;
    vec3 lit = mix(uHorizonColor.rgb, uSunColor.rgb + uSunGlowColor.rgb,
                   0.35);
    float tone = step(0.55, body * 0.6 + vUv.y * 0.5); // cel: 2 tones
    vec3 color = mix(dark, lit, tone * 0.65 + sunSide * 0.35);
    // Silver lining: bright rim where the mask thins toward the sun.
    float rim = smoothstep(0.34, 0.20, mask) * sunSide;
    color += (uSunColor.rgb + vec3(0.25)) * rim * 0.35;

    // Clouds live ABOVE the ground haze: cap the fog dissolve at 45 %
    // (full applyFog at 3.2 km reached ~98 % and erased the towers
    // entirely — dev report 2026-07-10). The partial haze keeps them
    // atmospheric without hiding them.
    vec3 fogged = applyFog(color, vWorldPos);
    fragColor = vec4(mix(color, fogged, 0.45), alpha);
}
