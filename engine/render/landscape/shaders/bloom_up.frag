#version 460 core

layout(binding = 0) uniform sampler2D uSource;

in vec2 vUv;
out vec4 fragColor;

// Tent upsample, ADDED into the target level (additive pipeline): each level
// spreads its glow into the finer one on the way back up.
void main() {
    vec2 texel = 1.0 / vec2(textureSize(uSource, 0));
    vec3 sum = texture(uSource, vUv).rgb * 4.0;
    sum += texture(uSource, vUv + texel * vec2(-1.0, 0.0)).rgb * 2.0;
    sum += texture(uSource, vUv + texel * vec2(1.0, 0.0)).rgb * 2.0;
    sum += texture(uSource, vUv + texel * vec2(0.0, -1.0)).rgb * 2.0;
    sum += texture(uSource, vUv + texel * vec2(0.0, 1.0)).rgb * 2.0;
    sum += texture(uSource, vUv + texel * vec2(-1.0, -1.0)).rgb;
    sum += texture(uSource, vUv + texel * vec2(1.0, -1.0)).rgb;
    sum += texture(uSource, vUv + texel * vec2(-1.0, 1.0)).rgb;
    sum += texture(uSource, vUv + texel * vec2(1.0, 1.0)).rgb;
    fragColor = vec4(sum / 16.0 * 0.85, 1.0);
}
