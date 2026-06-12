#version 450

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;

layout (set = 0, binding = 0) uniform sampler2D samplerScene;
layout (set = 0, binding = 1) uniform sampler2D samplerBloom;

layout (push_constant) uniform PushConstants {
    float bloomStrength;
    float exposure;
} params;

void main() {
    vec3 sceneColor = texture(samplerScene, inUV).rgb;
    vec3 bloomColor = texture(samplerBloom, inUV).rgb;

    // Additive blend
    vec3 result = sceneColor + bloomColor * params.bloomStrength;

    // Simple tone mapping (Reinhard)
    result = result / (result + vec3(1.0));

    // Exposure adjustment
    result = vec3(1.0) - exp(-result * params.exposure);

    // Gamma correction
    result = pow(result, vec3(1.0 / 2.2));

    outColor = vec4(result, 1.0);
}
