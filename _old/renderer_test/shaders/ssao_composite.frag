#version 450

// SSAO Composite Fragment Shader
// Multiplies scene color by SSAO factor

layout (binding = 0) uniform sampler2D samplerScene;  // Scene color
layout (binding = 1) uniform sampler2D samplerSSAO;   // SSAO (single channel)

layout (push_constant) uniform PushConstants {
    int ssaoOnly;
} pushConstants;

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;

void main() {
    vec3 sceneColor = texture(samplerScene, inUV).rgb;
    float ssao = texture(samplerSSAO, inUV).r;

    if (pushConstants.ssaoOnly != 0) {
        // Debug mode: show SSAO only
        outColor = vec4(vec3(ssao), 1.0);
    } else {
        // Apply SSAO to scene color
        outColor = vec4(sceneColor * ssao, 1.0);
    }
}

