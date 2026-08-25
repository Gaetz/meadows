#version 460 core
#include "common.glsl"

layout(location = 0) in vec2 aGrid; // unit quad [-1,1]²

layout(location = 0) out vec3 vWorldPos;

void main() {
    // A sea sheet following the camera, snapped to the chunk grid so the
    // wave field (a pure function of world position) never swims. Sized
    // to the FAR TERRAIN's span (FarTerrain::kSpan / 2): the far mesh
    // renders the analytic seafloor to the horizon, and everything below
    // sea level must stay UNDER water out there — a shorter sheet showed
    // the bare seabed as phantom land beyond its edge (measured in
    // game). Waves flatten with distance (ripple LOD), so the single
    // per-pixel quad scales fine.
    vec2 center = floor(uCameraPos.xz / 64.0) * 64.0;
    vec3 world = vec3(center.x + aGrid.x * 9000.0, uTerrainInfo.x,
                      center.y + aGrid.y * 9000.0);
    vWorldPos = world;
    gl_Position = uViewProj * vec4(world, 1.0);
}
