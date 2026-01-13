#version 450

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;

layout (set = 0, binding = 0) uniform sampler2D samplerScene;

layout (push_constant) uniform PushConstants {
    float threshold;
    float intensity;
} params;

void main() {
    vec3 color = texture(samplerScene, inUV).rgb;

    // Calculate luminance
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));

    // Extract bright areas
    if (luminance > params.threshold) {
        outColor = vec4(color * params.intensity, 1.0);
    } else {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
    }
}
