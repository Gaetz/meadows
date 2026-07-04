#version 460 core
#include "common.glsl"
#include "sky.glsl"

// Splat array, layers: 0 grass, 1 rock, 2 snow, 3 sand (SplatTextures.hpp).
layout(binding = 0) uniform sampler2DArray uSplat;
layout(binding = 1) uniform sampler2DArrayShadow uShadowMap;
#include "shadow.glsl"
#include "clouds.glsl"
#include "stylized.glsl"

in vec3 vNormal;
in vec3 vColor;
in vec3 vWorldPos;

out vec4 fragColor;

void main() {
    vec3 n = normalize(vNormal);
    float h = vWorldPos.y;
    float slope = 1.0 - n.y;
    vec2 uv = vWorldPos.xz * uTerrainInfo.z;
    float seaLevel = uTerrainInfo.x;
    float snowLine = uTerrainInfo.y;

    // Per-pixel material weights: rock claims slopes, snow the high flats,
    // sand the shoreline band, grass everything else. Smoothsteps give the
    // soft blended transitions; altitude borders are perturbed by a
    // low-frequency sample of the splat tiles so the sand and snow lines
    // wander organically instead of tracing a level contour.
    float wander = texture(uSplat, vec3(uv * 0.06, 0.0)).g - 0.5;

    float rockW = smoothstep(0.18, 0.35, slope);
    float snowH = h + wander * 26.0;
    float snowW = smoothstep(snowLine - 12.0, snowLine + 42.0, snowH) *
                  (1.0 - smoothstep(0.25, 0.45, slope));
    float sandH = h + wander * 5.0;
    float sandW = (1.0 - smoothstep(seaLevel + 1.0, seaLevel + 8.0, sandH)) *
                  (1.0 - rockW);
    float grassW = max(1.0 - rockW - snowW - sandW, 0.0);
    float total = grassW + rockW + snowW + sandW;

    vec3 albedo = (texture(uSplat, vec3(uv, 0.0)).rgb * grassW +
                   texture(uSplat, vec3(uv, 1.0)).rgb * rockW +
                   texture(uSplat, vec3(uv, 2.0)).rgb * snowW +
                   texture(uSplat, vec3(uv, 3.0)).rgb * sandW) /
                  total;

    albedo *= cascadeDebugTint(vWorldPos);
    float ndl = dot(n, uSunDirection.xyz);
    float diffuse = stylizedDiffuse(ndl, max(ndl, 0.0));
    // Cast shadows quantize to flat pools; cloud shadows stay soft (they
    // drift — hard edges would crawl).
    float shadow = stylizedShadow(shadowFactor(vWorldPos, n)) *
                   cloudShadowFactor(vWorldPos);
    vec3 lit =
        albedo * (uAmbientColor.rgb + uSunColor.rgb * (diffuse * shadow));
    fragColor = vec4(applyFog(lit, vWorldPos), 1.0);
}
