#version 460 core
#include "common.glsl"

layout(binding = 0) uniform sampler2D uSceneColor;
layout(binding = 1) uniform sampler2D uBloom;
layout(binding = 2) uniform sampler2D uGodRays;
layout(binding = 3) uniform sampler2D uVolumetric;
layout(binding = 4) uniform sampler2D uSsao;

in vec2 vUv;
out vec4 fragColor;

// ACES-fitted filmic curve (Krzysztof Narkowicz).
vec3 acesFilm(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    // Debug buffer viewer (uTime.w): 1 = bloom, 2 = god rays,
    // 3 = volumetric, 4 = SSAO.
    if (uTime.w > 0.5) {
        vec3 debugColor = uTime.w < 1.5   ? texture(uBloom, vUv).rgb * 2.0
                          : uTime.w < 2.5 ? texture(uGodRays, vUv).rgb * 2.0
                          : uTime.w < 3.5 ? texture(uVolumetric, vUv).rgb * 2.0
                                          : texture(uSsao, vUv).rrr;
        fragColor = vec4(pow(debugColor, vec3(1.0 / 2.2)), 1.0);
        return;
    }

    vec3 hdr = texture(uSceneColor, vUv).rgb;
    // SSAO first (uTime.y = strength): contact darkening belongs to the
    // scene's surfaces, not to the added airlight below.
    vec2 aoTexel = 1.0 / vec2(textureSize(uSsao, 0));
    float ao = (texture(uSsao, vUv + aoTexel * vec2(0.6, 0.2)).r +
                texture(uSsao, vUv + aoTexel * vec2(-0.2, 0.6)).r +
                texture(uSsao, vUv + aoTexel * vec2(-0.6, -0.2)).r +
                texture(uSsao, vUv + aoTexel * vec2(0.2, -0.6)).r) *
               0.25;
    hdr *= mix(1.0, ao, uTime.y);
    // Volumetric: alpha REMOVES the fog in-scatter where distant air is
    // cloud-shadowed (dark far curtains), rgb ADDS the near shafts. Then
    // bloom (uPostInfo.w) and god rays (uSunScreen.w), all in linear HDR
    // before exposure and the filmic curve.
    // Rotated-grid 4-tap fetch (16 effective bilinear samples) smooths the
    // marching dither out of both the shafts and the dark curtains.
    vec2 volTexel = 1.0 / vec2(textureSize(uVolumetric, 0));
    vec4 volumetric =
        (texture(uVolumetric, vUv + volTexel * vec2(0.6, 0.2)) +
         texture(uVolumetric, vUv + volTexel * vec2(-0.2, 0.6)) +
         texture(uVolumetric, vUv + volTexel * vec2(-0.6, -0.2)) +
         texture(uVolumetric, vUv + volTexel * vec2(0.2, -0.6))) *
        0.25;
    hdr = hdr * volumetric.a + volumetric.rgb;
    hdr += texture(uBloom, vUv).rgb * uPostInfo.w;
    hdr += texture(uGodRays, vUv).rgb * uSunScreen.w;
    hdr *= uPostInfo.y;
    // Submerged camera: the whole frame breathes water — teal absorption
    // that deepens with how far below the surface the camera sits.
    float submersion = clamp((uTerrainInfo.x - uCameraPos.y) * 0.35, 0.0,
                             1.0);
    hdr = mix(hdr, hdr * vec3(0.18, 0.55, 0.60) + vec3(0.004, 0.030, 0.036),
              submersion * 0.85);
    // A/B toggle: raw path clips instead of rolling off (same gamma encode,
    // so the comparison isolates the tonemap curve).
    vec3 color = uPostInfo.x > 0.5 ? acesFilm(hdr) : clamp(hdr, 0.0, 1.0);
    // Manual gamma encode — no global GL_FRAMEBUFFER_SRGB, the 2D sprite
    // path stays untouched.
    color = pow(color, vec3(1.0 / 2.2));
    fragColor = vec4(color, 1.0);
}
