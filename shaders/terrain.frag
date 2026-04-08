#version 450

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in float inHeight;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform TerrainUBO {
    mat4  viewProj;
    vec4  cameraPos;
    vec4  sunDir;
    float patchSize;
    float heightScale;
    float fbmFrequency;
    float fbmPersistence;
    int   fbmOctaves;
    int   gridSize;
    float ambientLight;
    float _pad;
} ubo;

void main() {
    float slope = 1.0 - inNormal.y;

    // Height-based colour gradient: deep ocean → sand → grass → rock → snow
    vec3 col = mix(vec3(0.05, 0.18, 0.45), vec3(0.76, 0.70, 0.50), smoothstep(0.00, 0.08, inHeight));
    col = mix(col, vec3(0.20, 0.45, 0.12), smoothstep(0.08, 0.18, inHeight));
    col = mix(col, vec3(0.45, 0.40, 0.35), smoothstep(0.50, 0.65, inHeight));
    col = mix(col, vec3(0.92, 0.95, 1.00), smoothstep(0.70, 0.85, inHeight));

    // Slope override: steep faces become rocky
    col = mix(col, vec3(0.45, 0.40, 0.35), smoothstep(0.25, 0.50, slope));

    // Lambert diffuse lighting
    float diff = max(dot(inNormal, normalize(ubo.sunDir.xyz)), 0.0);
    col *= ubo.ambientLight + (1.0 - ubo.ambientLight) * diff;

    // Atmospheric distance fog
    float dist = length(inWorldPos - ubo.cameraPos.xyz);
    float fog  = smoothstep(300.0, 700.0, dist);
    col = mix(col, vec3(0.68, 0.78, 0.88), fog);

    outColor = vec4(col, 1.0);
}
