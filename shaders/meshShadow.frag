#version 450

#extension GL_GOOGLE_include_directive : require
#include "inputStructures.glsl"

layout (set = 0, binding = 1) uniform sampler2D shadowMap;

layout (location = 0) in vec3 inNormal;
layout (location = 1) in vec3 inColor;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec4 inShadowCoord;

layout (location = 0) out vec4 outFragColor;

#define AMBIENT_SHADOW 0.3

float textureProj(vec4 shadowCoord, vec2 off)
{
    float shadow = 1.0;
    if (shadowCoord.z > -1.0 && shadowCoord.z < 1.0)
    {
        float dist = texture(shadowMap, shadowCoord.st + off).r;
        if (shadowCoord.w > 0.0 && dist < shadowCoord.z)
        {
            shadow = AMBIENT_SHADOW;
        }
    }
    return shadow;
}

float filterPCF(vec4 sc)
{
    ivec2 texDim = textureSize(shadowMap, 0);
    float scale = 1.5;
    float dx = scale * 1.0 / float(texDim.x);
    float dy = scale * 1.0 / float(texDim.y);

    float shadowFactor = 0.0;
    int count = 0;
    int range = 1;  // 3x3 kernel

    for (int x = -range; x <= range; x++)
    {
        for (int y = -range; y <= range; y++)
        {
            shadowFactor += textureProj(sc, vec2(dx*x, dy*y));
            count++;
        }
    }
    return shadowFactor / count;
}

void main()
{
    // Perform perspective divide for shadow coordinate
    vec4 shadowCoord = inShadowCoord / inShadowCoord.w;

    // Check if PCF is enabled (shadowParams.z > 0.5)
    float shadow = (sceneData.shadowParams.z > 0.5)
        ? filterPCF(shadowCoord)
        : textureProj(shadowCoord, vec2(0.0));

    // Calculate lighting
    float lightValue = max(dot(normalize(inNormal), sceneData.sunlightDirection.xyz), 0.1f);

    vec3 color = inColor * texture(colorTex, inUV).xyz;
    vec3 ambient = color * sceneData.ambientColor.xyz;

    // Apply shadow to diffuse lighting
    vec3 diffuse = color * lightValue * sceneData.sunlightColor.w * shadow;

    outFragColor = vec4(diffuse + ambient, 1.0f);
}
