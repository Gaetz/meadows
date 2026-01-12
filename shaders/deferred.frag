#version 450

#extension GL_GOOGLE_include_directive : require

#include "inputStructures.glsl"

layout (location = 0) in vec2 inUV;

layout (location = 0) out vec4 outColor;

// G-Buffer textures at set 2 (since set 0 is SceneData and set 1 is MaterialData in inputStructures.glsl)
// Wait, I should probably define a custom set for G-Buffer to avoid conflicts or just use set 2.
layout (set = 2, binding = 0) uniform sampler2D samplerPosition;
layout (set = 2, binding = 1) uniform sampler2D samplerNormal;
layout (set = 2, binding = 2) uniform sampler2D samplerAlbedo;

layout (push_constant) uniform constants {
    uint debugMode;
} pushConstants;

void main() {
    // Get G-Buffer data
    vec3 fragPos = texture(samplerPosition, inUV).rgb;
    vec3 normal = normalize(texture(samplerNormal, inUV).rgb);
    vec4 albedo = texture(samplerAlbedo, inUV);


    // Ambient
    vec3 ambient = sceneData.ambientColor.rgb * albedo.rgb;

    // Diffuse
    vec3 L = normalize(sceneData.sunlightDirection.xyz);
    float dotNH = max(0.0, dot(normal, L));
    vec3 diffuse = dotNH * sceneData.sunlightColor.rgb * albedo.rgb;

    // Final color
    vec3 color = ambient + diffuse;

    // Debug modes
    if (pushConstants.debugMode == 1) { // Position
        outColor = vec4(fragPos, 1.0);
    } else if (pushConstants.debugMode == 2) { // Normal
        outColor = vec4(normal * 0.5 + 0.5, 1.0);
    } else if (pushConstants.debugMode == 3) { // Albedo
        outColor = vec4(albedo.rgb, 1.0);
    } else {
        outColor = vec4(color, 1.0);
    }
}
