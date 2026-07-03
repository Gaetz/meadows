#version 460 core
#include "common.glsl"

in vec3 vNormal;
in vec3 vColor;
in vec3 vWorldPos;

out vec4 fragColor;

void main() {
    vec3 n = normalize(vNormal);
    float ndl = max(dot(n, uSunDirection.xyz), 0.0);
    vec3 lit = vColor * (uAmbientColor.rgb + uSunColor.rgb * ndl);
    fragColor = vec4(lit, 1.0);
}
