#version 450

#extension GL_GOOGLE_include_directive : require

#include "inputStructures.glsl"

layout (location = 0) in vec2 inUV;

layout (location = 0) out vec4 outColor;

// G-Buffer textures at set 2
layout (set = 2, binding = 0) uniform sampler2D samplerPosition;
layout (set = 2, binding = 1) uniform sampler2D samplerNormal;
layout (set = 2, binding = 2) uniform sampler2D samplerAlbedo;

// Point light structure
struct PointLight {
    vec4 position;    // xyz = position, w = unused
    vec4 colorRadius; // xyz = color, w = radius
};

// Lights uniform buffer at set 2, binding 3
layout (set = 2, binding = 3) uniform LightsData {
    PointLight lights[6];
    vec4 viewPos;
    int numLights;
} lightsData;

layout (push_constant) uniform constants {
    uint debugMode;
} pushConstants;

void main() {
    // Get G-Buffer data
    vec3 fragPos = texture(samplerPosition, inUV).rgb;
    vec3 normal = normalize(texture(samplerNormal, inUV).rgb);
    vec4 albedo = texture(samplerAlbedo, inUV);

    // Skip lighting for background pixels (position is 0,0,0)
    if (length(fragPos) < 0.001) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Ambient
    vec3 ambient = sceneData.ambientColor.rgb * albedo.rgb;

    // Accumulate lighting from all point lights
    vec3 lighting = vec3(0.0);

    for (int i = 0; i < lightsData.numLights; i++) {
        vec3 lightPos = lightsData.lights[i].position.xyz;
        vec3 lightColor = lightsData.lights[i].colorRadius.xyz;
        float lightRadius = lightsData.lights[i].colorRadius.w;

        // Calculate distance and attenuation
        vec3 L = lightPos - fragPos;
        float dist = length(L);
        L = normalize(L);

        // Attenuation based on radius
        float attenuation = max(0.0, 1.0 - dist / lightRadius);
        attenuation *= attenuation; // Quadratic falloff

        // Diffuse
        float NdotL = max(0.0, dot(normal, L));
        vec3 diffuse = NdotL * lightColor * albedo.rgb * attenuation;

        // Specular (Blinn-Phong)
        vec3 viewDir = normalize(lightsData.viewPos.xyz - fragPos);
        vec3 halfDir = normalize(L + viewDir);
        float NdotH = max(0.0, dot(normal, halfDir));
        float spec = pow(NdotH, 32.0);
        vec3 specular = spec * lightColor * attenuation * 0.5;

        lighting += diffuse + specular;
    }

    // Final color
    vec3 color = ambient + lighting;

    // Debug modes
    if (pushConstants.debugMode == 1) { // Position
        outColor = vec4(fragPos * 0.1, 1.0); // Scale down for visualization
    } else if (pushConstants.debugMode == 2) { // Normal
        outColor = vec4(normal * 0.5 + 0.5, 1.0);
    } else if (pushConstants.debugMode == 3) { // Albedo
        outColor = vec4(albedo.rgb, 1.0);
    } else if (pushConstants.debugMode == 4) { // Depth
        // Transform position to view space to get linear depth
        vec4 viewSpacePos = sceneData.view * vec4(fragPos, 1.0);
        float linearDepth = -viewSpacePos.z;
        // Normalize depth for visualization
        float near = 0.1;
        float far = 200.0;
        float depthVis = clamp((linearDepth - near) / (far - near), 0.0, 1.0);
        outColor = vec4(vec3(depthVis), 1.0);
    } else {
        outColor = vec4(color, 1.0);
    }
}
