#version 460 core
#include "common.glsl"

layout(location = 0) in vec2 aGrid; // unit quad [-1,1]²

layout(location = 0) out vec3 vWorldPos;

void main() {
    // A 3.2 km sheet following the camera, snapped to the chunk grid so the
    // wave field (a pure function of world position) never swims.
    vec2 center = floor(uCameraPos.xz / 64.0) * 64.0;
    vec3 world = vec3(center.x + aGrid.x * 1600.0, uTerrainInfo.x,
                      center.y + aGrid.y * 1600.0);
    vWorldPos = world;
    gl_Position = uViewProj * vec4(world, 1.0);
}
