#version 460 core

layout(binding = 0) uniform sampler2D uSource;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

// 4-tap box downsample (bilinear taps at the corner midpoints).
void main() {
    vec2 texel = 1.0 / vec2(textureSize(uSource, 0));
    vec3 sum = texture(uSource, vUv + texel * vec2(-1.0, -1.0)).rgb +
               texture(uSource, vUv + texel * vec2(1.0, -1.0)).rgb +
               texture(uSource, vUv + texel * vec2(-1.0, 1.0)).rgb +
               texture(uSource, vUv + texel * vec2(1.0, 1.0)).rgb;
    fragColor = vec4(sum * 0.25, 1.0);
}
