#version 460 core
#include "common.glsl"

// SSDM half-mode upsample: rewrites ONLY the full-res pixels the
// half-res resolve marked as moved (alpha), so the untouched scene
// keeps its native sharpness. The relief flag of the offscreen alpha
// is restored from the pre-warp full-res copy (the field is smooth —
// the SSAO tonemap weights never see the swap).
layout(binding = 0) uniform sampler2D uHalf;
layout(binding = 1) uniform sampler2D uSceneColor;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

void main() {
    vec4 warped = texture(uHalf, vUv);
    if (warped.a < 0.5) {
        discard; // untouched: keep the full-res pixel
    }
    fragColor = vec4(warped.rgb, texture(uSceneColor, vUv).a);
}
