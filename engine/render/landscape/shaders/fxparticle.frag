#version 460 core
#include "common.glsl"

// Chantier P0 C1 — soft round particle: radial falloff in the shader
// (no texture until an asset asks; ParticleForm.texture waits there).

in vec2 vUv;
in vec4 vColor;
out vec4 fragColor;

void main() {
    float r = length(vUv);
    float falloff = 1.0 - smoothstep(0.35, 1.0, r);
    float alpha = vColor.a * falloff;
    if (alpha <= 0.004) {
        discard;
    }
    fragColor = vec4(vColor.rgb, alpha);
}
