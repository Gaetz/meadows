#version 460 core
#include "common.glsl"

in vec3 vNormal;
in vec3 vColor;
in vec3 vWorldPos;

out vec4 fragColor;

void main() {
    // Fixed sun until the SkySystem drives it through FrameUbo (brick 9).
    vec3 sunDir = normalize(vec3(0.35, 0.75, 0.25));
    float ndl = max(dot(normalize(vNormal), sunDir), 0.0);
    vec3 lit = vColor * (0.35 + 0.75 * ndl);
    fragColor = vec4(lit, 1.0);
}
