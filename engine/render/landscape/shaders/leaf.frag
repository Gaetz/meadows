#version 460 core
#include "common.glsl"
#include "sky.glsl"

layout(binding = 1) uniform sampler2DArrayShadow uShadowMap;
#include "shadow.glsl"
#include "clouds.glsl"

layout(binding = 4) uniform sampler2D uLeafTex;

in vec3 vNormal;
in vec3 vColor;
in vec3 vWorldPos;
in vec2 vUv;
in float vTint;

out vec4 fragColor;

void main() {
    // The article's wind: displace the sampled UVs so the leaf SHAPES
    // wiggle inside the card (organic shimmer), on top of the vertex sway.
    // Amplitude stays well under the atlas cell padding.
    vec2 wiggle =
        vec2(sin(uWindInfo.x * 2.3 + vWorldPos.x * 1.9 + vWorldPos.y * 0.7),
             cos(uWindInfo.x * 2.7 + vWorldPos.z * 2.1)) *
        (0.010 * uWindInfo.y);
    vec4 bouquet = texture(uLeafTex, vUv + wiggle);
    // Cutout. The threshold sits below 0.5 to offset alpha erosion in the
    // mip chain at distance.
    if (bouquet.a < 0.38) {
        discard;
    }

    // Near-flat atlas luminance; instance hue roll matches the body shader
    // so cards and blobs stay one tree.
    vec3 albedo = vColor * (bouquet.rgb * 1.35) *
                  mix(vec3(0.85, 1.0, 0.75), vec3(1.1, 1.0, 1.15), vTint);
    albedo *= cascadeDebugTint(vWorldPos);

    // The article's cel lighting: ONE crisp cutoff on N·L of the spherical
    // normal — the sphere gradient turns into a clean lit/shade boundary
    // sweeping the canopy. The narrow smoothstep is only anti-aliasing.
    vec3 n = normalize(vNormal);
    float ndl = dot(n, uSunDirection.xyz);
    float cel = smoothstep(0.02, 0.14, ndl);

    // Backlit translucency (ported from the grass): sun shining through
    // the canopy toward the camera.
    vec3 viewDir = normalize(vWorldPos - uCameraPos.xyz);
    float backlight =
        pow(max(dot(viewDir, uSunDirection.xyz), 0.0), 3.0) * 0.30;

    float shadow = shadowFactor(vWorldPos, n) * cloudShadowFactor(vWorldPos);
    // Shade side = cool ambient only (that IS the toon shadow color);
    // lit side adds the full sun band.
    vec3 lit = albedo * (uAmbientColor.rgb * (0.9 + 0.25 * n.y) +
                         uSunColor.rgb * ((cel * 0.95 + backlight) * shadow));

    fragColor = vec4(applyFog(lit, vWorldPos), 1.0);
}
