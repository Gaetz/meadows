#version 460 core
#include "common.glsl"

// Brick 29 (chantier 6 B4): the eye-adaptation micro-pass. Reads the 1x1
// log-average luminance mip + the previous exposure (1x1 ping-pong) and
// writes the new exposure with asymmetric inertia — adapting to darkness
// (exposure rising) is slower than to a bright scene, like an eye.
// Slots: uSunDirection.w = frame dt; uHorizonFarColor.w / uCloudMapInfo.w
// = exposure min / max (LandscapeTuningForm).

layout(binding = 0) uniform sampler2D uLuminance;
layout(binding = 1) uniform sampler2D uPrevExposure;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

void main() {
    // Mip 6 of a 64x64 pyramid = the 1x1 log-average.
    float avgLum = exp(texelFetch(uLuminance, ivec2(0), 6).r);
    // Reinhard key: the average maps to middle grey.
    float target = clamp(0.18 / max(avgLum, 1e-4),
                         uHorizonFarColor.w, uCloudMapInfo.w);
    float prev = texelFetch(uPrevExposure, ivec2(0), 0).r;
    if (prev < 1e-3) {
        // First frame (or the toggle just flipped on): no history — snap.
        fragColor = vec4(target, 0.0, 0.0, 1.0);
        return;
    }
    float dt = uSunDirection.w;
    float speed = target > prev ? 1.2 : 3.0; // darkness adapts slower
    float exposure = prev + (target - prev) * (1.0 - exp(-dt * speed));
    fragColor = vec4(exposure, 0.0, 0.0, 1.0);
}
