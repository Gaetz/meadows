#version 460 core
#include "common.glsl"

// Flame tongues (docs/FIRE-RENDER.md F2): a fluid-simulation FLIPBOOK
// (ParticleForm.texture, columns x rows frames, uFireFlameInfo) on an
// upright quad anchored at the particle — two frames blended (the motion
// is the sim's own, no motion vectors needed at this cadence), the sheet
// linearized from sRGB, tinted by the particle's core colour, HDR-boosted
// (uFireFlameLook.x, the bloom bites), optionally posterized into bands
// of brightness (uFireFlameLook.y), born fast and eroded away over the
// last third of the life. A flame particle without a sheet is drawn by
// the plain sprite pipeline instead (FxRenderer).

layout(binding = 3) uniform sampler2D uFlameSheet;

layout(location = 0) in vec2 vUv;    // x -1..1 across, y 0 base .. 1 tip
layout(location = 1) in vec4 vCore;  // rgb tint, a = age 0..1
layout(location = 2) in vec4 vOuter; // a = seed 0..1 (rgb unused here)
layout(location = 3) in vec4 vLife;  // x = lifetime (s)
layout(location = 0) out vec4 fragColor;

vec4 flipbook(vec2 uv, float age, float lifetime, float seed) {
    float cols = uFireFlameInfo.x;
    float rows = uFireFlameInfo.y;
    float frames = cols * rows;
    float f = seed * frames + age * lifetime * uFireFlameInfo.z;
    float f0 = mod(floor(f), frames);
    float f1 = mod(f0 + 1.0, frames);
    float blend = fract(f);
    // The sheet's rows run top-down: the base is the frame's bottom.
    vec2 local = vec2(uv.x * 0.5 + 0.5, 1.0 - uv.y);
    vec2 cell = vec2(1.0 / cols, 1.0 / rows);
    vec2 uv0 = (vec2(mod(f0, cols), floor(f0 / cols)) + local) * cell;
    vec2 uv1 = (vec2(mod(f1, cols), floor(f1 / cols)) + local) * cell;
    return mix(texture(uFlameSheet, uv0), texture(uFlameSheet, uv1), blend);
}

void main() {
    float seed = vOuter.a;
    float age = vCore.a;
    vec4 s = flipbook(vUv, age, max(vLife.x, 0.01), seed);
    vec3 col = pow(max(s.rgb, vec3(0.0)), vec3(2.2)) * vCore.rgb * uFireFlameLook.x;
    float posterize = uFireFlameLook.y;
    if (posterize >= 1.0) {
        float lum = dot(col, vec3(0.299, 0.587, 0.114));
        float q = floor(lum * posterize + 0.5) / posterize;
        col *= lum > 1e-4 ? q / lum : 0.0;
    }
    float fade = smoothstep(0.0, 0.12, age) * (1.0 - smoothstep(0.65, 1.0, age));
    float alpha = s.a * fade;
    if (alpha <= 0.004) {
        discard;
    }
    fragColor = vec4(col, alpha);
}
