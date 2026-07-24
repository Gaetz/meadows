#version 460 core
#include "common.glsl"

// Froxel fog, pass 3: fullscreen resolve into the SAME half-res target
// the 2D march writes — the tonemap's `scene x a + rgb` composite is
// untouched. One trilinear fetch of the integrated column at the pixel's
// depth (log slice mapping, matching sliceDepth in the compute passes).
layout(binding = 0) uniform sampler2D uSceneDepth;
layout(binding = 4) uniform sampler3D uFroxelIntegrated;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

void main() {
    float reach = uFogSunInfo.z;
    if (uTime.z <= 0.0 || reach <= 0.0) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    float depth = texture(uSceneDepth, vUv).r;
    vec4 ndc = vec4(vUv * 2.0 - 1.0, depth, 1.0);
    vec4 world = uInvViewProj * ndc;
    float d = distance(world.xyz / world.w, uCameraPos.xyz);

    const float kNear = 1.0;
    float far = max(reach, kNear + 1.0);
    float slice = clamp(log(max(d, kNear) / kNear) / log(far / kNear),
                        0.0, 1.0);
    fragColor = texture(uFroxelIntegrated, vec3(vUv, slice));
}
