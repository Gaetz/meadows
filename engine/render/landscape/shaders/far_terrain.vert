#version 460 core
#include "common.glsl"

// Distant landscape silhouettes (FarTerrain): the coarse far mesh,
// sunk under the exact near terrain inside the streaming ring
// (uFogLayerInfo.z = ring meters) so its 62 m sampling never pokes
// through; the bias fades out beyond the ring where the far mesh is
// the only geometry.
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
    p.y -= mix(12.0, 0.0,
               smoothstep(uFogLayerInfo.z * 0.85, uFogLayerInfo.z * 1.5,
                          d));
    vWorldPos = p;
    vNormal = aNormal;
    vColor = aColor;
    gl_Position = uViewProj * vec4(p, 1.0);
}
