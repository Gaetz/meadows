#version 460 core
#include "common.glsl"
#include "sky.glsl"

in float vT;
in float vSide;
in float vTint;
in vec3 vNormal;
in vec3 vWorldPos;

out vec4 fragColor;

void main() {
    // Linear-space palette, matched to the grass splat tile family: shaded
    // root climbing to a fresher tip, with per-blade hue jitter.
    vec3 baseColor = mix(vec3(0.020, 0.052, 0.010), vec3(0.032, 0.070, 0.014),
                         vTint);
    vec3 tipColor = mix(vec3(0.075, 0.160, 0.036), vec3(0.120, 0.180, 0.045),
                        vTint);
    vec3 albedo = mix(baseColor, tipColor, vT);

    // Grounded look: ambient occlusion at the root of the ribbon.
    float ao = mix(0.45, 1.0, vT);

    vec3 n = normalize(vNormal);
    // Wrap diffuse: soft, carpet-like response instead of hard lambert.
    float wrap = clamp((dot(n, uSunDirection.xyz) + 0.5) / 1.5, 0.0, 1.0);

    // Backlight translucency + a thin view-dependent sheen along the blade,
    // strongest near the tips — the glint that sells a windy meadow.
    vec3 viewDir = normalize(vWorldPos - uCameraPos.xyz);
    float backlight =
        pow(max(dot(viewDir, uSunDirection.xyz), 0.0), 3.0) * 0.30 * vT;
    vec3 halfDir = normalize(uSunDirection.xyz - viewDir);
    float sheen = pow(max(dot(n, halfDir), 0.0), 24.0) * 0.25 * vT;

    vec3 lit = albedo * ao *
                   (uAmbientColor.rgb + uSunColor.rgb * (wrap + backlight)) +
               uSunColor.rgb * sheen * ao;

    fragColor = vec4(applyFog(lit, vWorldPos), 1.0);
}
