#version 460 core
// FAR baked bodies (lakes + ribbons beyond the sim's trusted rect).
// WATER_LOCAL = the shared local-water machinery (materials, flow,
// torrent); WATER_FAR marks the far-only branches (no foam family —
// distance hides it, and the sim owns everything near).
#define WATER_LOCAL 1
#define WATER_FAR 1
#include "water_surface.glsl"
