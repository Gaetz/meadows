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
    // The sink GROWS with distance: 0..1 non-reversed depth loses
    // ~d^2*1.2e-6 meters of precision (near 0.1), so a constant bias
    // falls under the quantization noise kilometres out and the 62 m
    // quads z-fight the sea and the flats — the black low-res grid,
    // triangle by triangle. 8 mm/m keeps the mesh below the noise to
    // the horizon; at those distances the drop is angularly invisible.
    // (The systemic cure is reversed-Z — reserved in docs §1.2.)
    p.y += aUv.x * ringFade - mix(12.0, 0.0, ringFade) - d * 0.008;
    vWorldPos = p;
    vNormal = aNormal;
    vColor = aColor;
    gl_Position = uViewProj * vec4(p, 1.0);
}
