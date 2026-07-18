#version 460 core
#define CLOUD_BAKE_PASS
#include "common.glsl"
#include "clouds.glsl"

// Once per frame: evaluate the drifting cloud field analytically over the
// area around the camera (uCloudMapInfo) into a small R16F texture. Every
// shadow consumer then samples this instead of re-running the FBM.
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

void main() {
    vec2 planePos = uCloudMapInfo.xy + (vUv - 0.5) / uCloudMapInfo.z;
    fragColor = vec4(cloudDensityAnalytic(planePos), 0.0, 0.0, 1.0);
}
