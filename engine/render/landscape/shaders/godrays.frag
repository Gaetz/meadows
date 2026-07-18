#version 460 core
#include "common.glsl"

// Screen-space light shafts (GPU Gems 3 style): march from each pixel toward
// the sun's screen position, accumulating sky/sun radiance visible between
// occluders — trees and ridgelines carve the rays.
layout(binding = 0) uniform sampler2D uSceneColor;
layout(binding = 1) uniform sampler2D uSceneDepth;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

void main() {
    if (uSunScreen.z <= 0.001) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    const int kTaps = 48;
    vec2 delta = (uSunScreen.xy - vUv) / float(kTaps);
    vec2 uv = vUv;
    float illumination = 1.0;
    vec3 sum = vec3(0.0);
    for (int i = 0; i < kTaps; ++i) {
        uv += delta;
        vec2 clamped = clamp(uv, vec2(0.0), vec2(1.0));
        // Only unoccluded sky feeds the shafts (depth at the far plane).
        float sky = step(0.99995, texture(uSceneDepth, clamped).r);
        sum += texture(uSceneColor, clamped).rgb * (sky * illumination);
        illumination *= 0.955;
    }
    fragColor = vec4(sum / float(kTaps) * uSunScreen.z, 1.0);
}
