#version 460 core

// Brick 29 (chantier 6 B4): log-luminance of the HDR scene into a 64x64
// R16F target; generateMipmaps reduces it to the 1x1 log-average the
// adaptation pass reads.

layout(binding = 0) uniform sampler2D uSceneColor;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

void main() {
    vec3 hdr = texture(uSceneColor, vUv).rgb;
    float luma = dot(hdr, vec3(0.2126, 0.7152, 0.0722));
    fragColor = vec4(log(max(luma, 1e-4)), 0.0, 0.0, 1.0);
}
