// Radiance cascades shared helpers (G4/G5) — layouts and direction
// encoding. Two storage layouts (docs/RADIANCE-CASCADES.md §2.2):
//  - cascade 0, DIR-MAJOR "slabs": 3D texture res × res × (res·8);
//    z = dirIndex·res + probeZ — same-direction probes are contiguous,
//    so the APPLY (gi.glsl) gets hardware trilinear per direction.
//  - cascades 1+, DIR-TILED: 3D texture (P·tx) × (P·ty) × P; the xy
//    plane tiles the direction grid (tx × ty tiles of P × P probes);
//    z = probeZ. The merge interpolates probes manually anyway.
// Directions: octahedral full-sphere; cascade i has (2·2^i) × (4·2^i)
// direction texels — quadrupling per level, and a direction (dx, dy)
// of cascade i has exactly the 2×2 children (2dx+o, 2dy+o) in i+1.

// Octahedral decode: grid cell (d.x, d.y) of a gridW × gridH direction
// grid, texel center, to a unit direction.
vec3 rcOctDecode(vec2 cell, vec2 gridSize) {
    vec2 uv = (cell + 0.5) / gridSize;        // (0,1)
    vec2 e = uv * 2.0 - 1.0;                  // (-1,1)
    vec3 v = vec3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    if (v.y < 0.0) {
        vec2 sgn = vec2(v.x >= 0.0 ? 1.0 : -1.0, v.z >= 0.0 ? 1.0 : -1.0);
        vec2 flip = (1.0 - abs(vec2(v.z, v.x))) * sgn;
        v.x = flip.x;
        v.z = flip.y;
    }
    return normalize(v);
}

// Radiance interval bounds of cascade i (meters): geometric doubling.
float rcIntervalStart(int cascade, float interval0) {
    return interval0 * (exp2(float(cascade)) - 1.0);
}
float rcIntervalEnd(int cascade, float interval0) {
    return interval0 * (exp2(float(cascade) + 1.0) - 1.0);
}

// The sky seen along `dir` — the far-field seed merged into the TOP
// cascade (β still open at the end of the last interval = sky light).
// Cheap gradient from the frame's sky colors; ground hemisphere fades
// dark (the terrain light map already carries ground bounce macro).
vec3 rcSkyRadiance(vec3 dir) {
    float up = clamp(dir.y, -1.0, 1.0);
    vec3 sky = mix(uHorizonColor.rgb, uZenithColor.rgb,
                   clamp(up, 0.0, 1.0));
    return sky * clamp(0.15 + 0.85 * up, 0.0, 1.0);
}
