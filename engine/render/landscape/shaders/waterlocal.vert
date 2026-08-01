#version 460 core
#include "common.glsl"

// Local water surfaces (lakes at their own level, river ribbons):
// world-space vertices with the per-surface character the shared shading
// specializes on — flow vector (0 = still lake), half-width and lateral
// position in the ribbon (banks/rapids foam).
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aFlow; // direction * speed (m/s)
layout(location = 2) in vec4 aInfo; // halfWidth (0 = lake), lateral [-1,1], arc (m), endDist (m)
layout(location = 3) in float aMaterial; // WaterMaterialsUbo slot

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec2 vFlow;
layout(location = 2) out vec4 vInfo;
layout(location = 3) flat out float vMaterial;

void main() {
    vWorldPos = aPos;
    vFlow = aFlow;
    vInfo = aInfo;
    vMaterial = aMaterial;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
