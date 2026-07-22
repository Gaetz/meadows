#version 460 core
#include "common.glsl"

// (binding 4 is unused — no screen-space AO; grounding
// comes from the terrain light map, contact shadows and baked vertex AO.)
layout(binding = 0) uniform sampler2D uSceneColor;
layout(binding = 1) uniform sampler2D uBloom;
layout(binding = 2) uniform sampler2D uGodRays;
layout(binding = 3) uniform sampler2D uVolumetric;
layout(binding = 5) uniform sampler2D uExposure; // 1x1 adaptation
layout(binding = 6) uniform sampler2D uContact;  // white = lit

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

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
    // 3 = volumetric.
    if (uTime.w > 0.5) {
        vec3 debugColor = uTime.w < 1.5   ? texture(uBloom, vUv).rgb * 2.0
                          : uTime.w < 2.5 ? texture(uGodRays, vUv).rgb * 2.0
                                          : texture(uVolumetric, vUv).rgb * 2.0;
        fragColor = vec4(pow(debugColor, vec3(1.0 / 2.2)), 1.0);
        return;
    }

    vec3 hdr = texture(uSceneColor, vUv).rgb;
    // Screen-space contact shadows — surface darkening before
    // the added airlight below. The texture is the toggle: the scene
    // clears it to white when the feature is off.
    hdr *= texture(uContact, vUv).r;
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
    hdr *= uPostInfo.y; // exposure (becomes the EV bias when auto is on)
    // The adapted exposure, one tap. The scene
    // flags it on uWindInfo.w so the toggle costs nothing when off.
    if (uWindInfo.w > 0.5) {
        hdr *= texelFetch(uExposure, ivec2(0), 0).r;
    }
    // Submerged camera: the whole frame breathes water — teal absorption
    // that deepens with how far below the surface the camera sits. The
    // scene sends the EFFECTIVE surface: sea level outdoors, a
    // water volume's top when the camera is inside one (flooded rooms
    // included), -1e6 = dry (plain interiors — the greenish-interior bug
    // stays fixed by construction).
    float submersion =
        clamp((uSubmersionInfo.x - uCameraPos.y) * 0.35, 0.0, 1.0);
    hdr = mix(hdr, hdr * vec3(0.18, 0.55, 0.60) + vec3(0.004, 0.030, 0.036),
              submersion * 0.85);
    // A/B toggle: raw path clips instead of rolling off (same gamma encode,
    // so the comparison isolates the tonemap curve).
    vec3 color = uPostInfo.x > 0.5 ? acesFilm(hdr) : clamp(hdr, 0.0, 1.0);
    // Per-channel ACES skews saturated warm light toward yellow-green
    // as it brightens (R rolls off first, G catches up). INDOORS
    // (uCascadeSplits.w) blend toward a hue-preserving mapping: ACES on
    // the luminance, original color ratio kept. The exterior keeps the
    // classic curve — sunsets live off exactly that desaturation.
    if (uCascadeSplits.w > 0.5 && uPostInfo.x > 0.5) {
        float l = dot(hdr, vec3(0.2126, 0.7152, 0.0722));
        vec3 hueKept = hdr * (acesFilm(vec3(l)).r / max(l, 1e-4));
        color = mix(color, clamp(hueKept, 0.0, 1.0), 0.55);
    }
    // Analytical BotW grade between the curve and
    // the gamma encode. Parameters ride free .w slots — uSunGlowColor.w =
    // vibrance, uZenithColor.w = split-tone strength, uHorizonColor.w =
    // contrast; the scene sends neutral (0 / 0 / 1) when the toggle is off.
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float vibrance = uSunGlowColor.w;
    if (vibrance > 0.001) {
        // Weighted saturation: pale colors get the boost, already-vivid
        // ones barely move (greens sing without going neon at noon).
        float sat = max(color.r, max(color.g, color.b)) -
                    min(color.r, min(color.g, color.b));
        color = mix(vec3(luma), color, 1.0 + vibrance * (1.0 - sat));
    }
    float splitTone = uZenithColor.w;
    if (splitTone > 0.001) {
        vec3 shadowTint = vec3(-0.04, 0.00, 0.06);  // cool shadows
        vec3 highTint = vec3(0.06, 0.015, -0.05);   // warm highlights
        color += splitTone *
                 mix(shadowTint, highTint, smoothstep(0.15, 0.85, luma));
    }
    float contrast = uHorizonColor.w;
    if (contrast > 0.001) {
        color = mix(vec3(0.5), color, contrast); // pivot 0.5
    }
    color = clamp(color, 0.0, 1.0);
    // Manual gamma encode — no global GL_FRAMEBUFFER_SRGB, the 2D sprite
    // path stays untouched.
    color = pow(color, vec3(1.0 / 2.2));
    fragColor = vec4(color, 1.0);
}
