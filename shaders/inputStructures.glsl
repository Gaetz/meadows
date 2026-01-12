layout(set = 0, binding = 0) uniform  SceneData{
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 lightSpaceMatrix;  // Light view-projection for shadow mapping
    vec4 ambientColor;
    vec4 sunlightDirection; // w for sun power
    vec4 sunlightColor;
    vec4 shadowParams;      // x=zNear, y=zFar, z=enablePCF, w=shadowBias
} sceneData;

layout(set = 1, binding = 0) uniform GLTFMaterialData {
    vec4 colorFactors;
    vec4 metalRoughFactors;
} materialData;

layout(set = 1, binding = 1) uniform sampler2D colorTex;
layout(set = 1, binding = 2) uniform sampler2D metalRoughTex;
