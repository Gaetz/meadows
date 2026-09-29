#version 460 core
#include "common.glsl"

// Heat haze (docs/FIRE-RENDER.md F4): a camera-facing quad that shows the
// scene BEHIND it (the pre-fx colour snapshot, unit 0) through a
// rolling refraction — nothing is emitted, the air only wobbles. The
// offset is a scrolling value noise scaled by uFireFlameLook.z (metres
// of screen, the Fire panel's "Heat haze") and the quad's soft mask.
// Drawn after the flames, so the refracted image is the pre-flame scene
// (the haze sits above the tongues where that reads fine).

layout(binding = 0) uniform sampler2D uSceneColor;

layout(location = 0) in vec2 vUv;    // -1..1 across the quad
layout(location = 1) in vec4 vColor; // x = seed, y = age 0..1, z = strength
layout(location = 0) out vec4 fragColor;

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float valueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
    float r = length(vUv);
    float age = vColor.y;
    // Soft disc, born and dying gently.
    float mask = (1.0 - smoothstep(0.15, 1.0, r)) * vColor.z *
                 smoothstep(0.0, 0.2, age) * (1.0 - smoothstep(0.6, 1.0, age));
    if (mask <= 0.004) {
        discard;
    }
    float seed = vColor.x * 37.0;
    float t = uTime.x;
    // Two scrolling noise fields (up = the hot air rising).
    vec2 p = vUv * 2.5;
    vec2 n = vec2(valueNoise(p + vec2(seed, -t * 2.6)),
                  valueNoise(p + vec2(-t * 2.1, seed * 0.7 + 11.0))) - 0.5;
    vec2 screenUv = gl_FragCoord.xy * uScreenInfo.zw;
    vec2 uv = clamp(screenUv + n * uFireFlameLook.z * mask, vec2(0.001), vec2(0.999));
    fragColor = vec4(texture(uSceneColor, uv).rgb, mask);
}
