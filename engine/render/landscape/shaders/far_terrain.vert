#version 460 core
#include "common.glsl"

// Distant landscape silhouettes (FarTerrain). Inside the streaming ring
// (uFogLayerInfo.z, meters) the mesh must stay strictly UNDER the fine
// terrain: the bake min-samples each vertex over its quad support (a
// coarse triangle can then never rise above the true surface — a fixed
// sink could not absorb the 62 m interpolation overshoot on slopes) and
// a small residual sink covers sub-half-cell relief. What the min gave
// up — the true-height delta plus the forest canopy raise — rides
// aUv.x and is restored BEYOND the ring, where the far mesh is the only
// geometry and the crests must keep their real silhouettes.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec3 aColor;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vColor;

void main() {
    vec3 p = aPos;
    float d = distance(p.xz, uCameraPos.xz);
    float ringFade =
        smoothstep(uFogLayerInfo.z * 0.85, uFogLayerInfo.z * 1.5, d);
    // The sink GROWS with distance, then CLAMPS at 16 m: the growth
    // exists for the non-reversed GL fallback, whose depth noise at
    // range once z-fought the 62 m quads against the sea (the black
    // low-res grid). Reversed-Z (the shipped path) needs none of it,
    // and an unbounded 8 mm/m sank far coasts 192 m UNDER the sea quad
    // at 24 km and lowered every distant silhouette — so the slope is
    // kept only to its anti-fight range. GL past 2 km is an assumed
    // regression (frozen backend).
    p.y += aUv.x * ringFade - mix(12.0, 0.0, ringFade) -
           min(d, 2000.0) * 0.008;
    vWorldPos = p;
    vNormal = aNormal;
    vColor = aColor;
    gl_Position = uViewProj * vec4(p, 1.0);
}
