#version 450

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;

layout (set = 0, binding = 0) uniform sampler2D samplerColor;

layout (set = 0, binding = 1) uniform BlurParams {
    float blurScale;
    float blurStrength;
} params;

// Specialization constant for blur direction (0 = vertical, 1 = horizontal)
layout (constant_id = 0) const int blurDirection = 0;

void main() {
    // Gaussian weights
    float weight[5];
    weight[0] = 0.227027;
    weight[1] = 0.1945946;
    weight[2] = 0.1216216;
    weight[3] = 0.054054;
    weight[4] = 0.016216;

    vec2 texOffset = 1.0 / textureSize(samplerColor, 0) * params.blurScale;
    vec3 result = texture(samplerColor, inUV).rgb * weight[0];

    for (int i = 1; i < 5; ++i) {
        if (blurDirection == 1) {
            // Horizontal blur
            result += texture(samplerColor, inUV + vec2(texOffset.x * i, 0.0)).rgb * weight[i] * params.blurStrength;
            result += texture(samplerColor, inUV - vec2(texOffset.x * i, 0.0)).rgb * weight[i] * params.blurStrength;
        } else {
            // Vertical blur
            result += texture(samplerColor, inUV + vec2(0.0, texOffset.y * i)).rgb * weight[i] * params.blurStrength;
            result += texture(samplerColor, inUV - vec2(0.0, texOffset.y * i)).rgb * weight[i] * params.blurStrength;
        }
    }

    outColor = vec4(result, 1.0);
}
