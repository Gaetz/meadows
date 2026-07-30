#version 460 core
#include "common.glsl"
#include "sky.glsl"
#include "clouds.glsl"

// Distant landscape silhouettes (FarTerrain): flat vertex-color shading
// — sun N·L × cloud shadows + ambient — then the analytic fog does the
// real work: ridgelines and the raised forest fringe dissolve into the
// sky gradient. No CSM (beyond the cascades), no GI, no splat detail:
// at 1-10 km none of it would survive the veil.
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vColor;
layout(location = 0) out vec4 fragColor;

void main() {
    vec3 n = normalize(vNormal);
    float cloudVis = cloudShadowFactor(vWorldPos);
    float ndl = max(dot(n, uSunDirection.xyz), 0.0);
    vec3 lit =
        vColor * (uAmbientColor.rgb + uSunColor.rgb * (ndl * cloudVis));
    fragColor = vec4(applyFog(lit, vWorldPos, cloudVis), 1.0);
}
