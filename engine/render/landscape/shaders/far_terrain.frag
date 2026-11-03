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

// The resident near chunks (TerrainSystem): where the exact terrain is
// drawn the far backdrop is CUT — in a gorge its 62 m triangles, and
// the true heights restored toward the ring's edge, stood above the
// near floor and hid it. One bit per chunk of a side x side window at
// chunk (origin); side 0 = no cut (the A/B).
layout(std140, binding = 1) uniform FarResidencyUbo {
    vec4 uFarResidencyInfo; // xy = origin chunk, z = side, w = chunk m
    uvec4 uFarResidencyBits[96];
};

bool residentAt(vec2 world) {
    int side = int(uFarResidencyInfo.z);
    if (side <= 0) {
        return false;
    }
    ivec2 c = ivec2(floor(world / uFarResidencyInfo.w)) -
              ivec2(uFarResidencyInfo.xy);
    if (c.x < 0 || c.y < 0 || c.x >= side || c.y >= side) {
        return false;
    }
    int idx = c.y * side + c.x;
    uint word = uFarResidencyBits[idx >> 7][(idx >> 5) & 3];
    return ((word >> uint(idx & 31)) & 1u) != 0u;
}

void main() {
    if (residentAt(vWorldPos.xz)) {
        discard;
    }
    vec3 n = normalize(vNormal);
    float cloudVis = cloudShadowFactor(vWorldPos);
    float ndl = max(dot(n, uSunDirection.xyz), 0.0);
    vec3 lit =
        vColor * (uAmbientColor.rgb + uSunColor.rgb * (ndl * cloudVis));
    fragColor = vec4(applyFog(lit, vWorldPos, cloudVis), 1.0);
}
