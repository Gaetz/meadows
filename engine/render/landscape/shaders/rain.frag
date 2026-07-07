#version 460 core
#include "common.glsl"

// Brick 31 — rain streak: a soft translucent sliver tinted by the sky.

in float vAlpha;
out vec4 fragColor;

void main() {
    if (vAlpha <= 0.003) {
        discard;
    }
    vec3 color = mix(uAmbientColor.rgb * 2.0, uHorizonColor.rgb, 0.4) +
                 vec3(0.18);
    fragColor = vec4(color, vAlpha);
}
