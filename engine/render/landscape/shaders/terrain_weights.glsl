// Per-pixel splat material weights — THE weight rule, mirrored bit-for-bit
// by TerrainNoise.cpp materialWeightsCore (footsteps, scatter, GI stay in
// lockstep through that one function). The hybrid-C seam: region shading
// (biome/hardness) extends the INPUTS of this function and a future painted
// override composes on its RESULT — neither rewrites the rule.
// Layers: grass 0, rock 1, snow 2, sand 3, cliff 4 (SplatTextures.hpp).

struct TerrainWeights {
    float grass;
    float rock;
    float snow;
    float sand;
    float cliff;
};

// Deterministic lattice value noise for the snow/sand border wander —
// MATERIAL-SET INDEPENDENT (the old grass-albedo green tap broke the
// calibration the day cooked photos replaced the procedural tiles) and
// mirrored bit-for-bit by TerrainNoise.cpp borderWander (murmur finalizer
// = core::hashU32). -0.31 is the historical tuning bias the snow/sand
// smoothsteps are tuned against; the ±0.1 wiggle makes the altitude
// borders wander instead of tracing a level contour.
uint wanderHash(uint v) {
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}

float wanderLattice(int x, int y) {
    uint h = wanderHash(uint(x) * 0x9e3779b9u ^ uint(y) * 0x85ebca6bu);
    return float(h) * (1.0 / 4294967295.0);
}

float wanderValue01(vec2 p) {
    vec2 f = floor(p);
    vec2 t = p - f;
    vec2 u = t * t * (3.0 - 2.0 * t);
    int x0 = int(f.x);
    int y0 = int(f.y);
    float v00 = wanderLattice(x0, y0);
    float v10 = wanderLattice(x0 + 1, y0);
    float v01 = wanderLattice(x0, y0 + 1);
    float v11 = wanderLattice(x0 + 1, y0 + 1);
    return mix(mix(v00, v10, u.x), mix(v01, v11, u.x), u.y);
}

float borderWander(vec2 p) {
    return -0.31 + (wanderValue01(p) - 0.5) * 0.2;
}

// Snow-patch field [0,1] (world meters): two incommensurate octaves of
// the wander lattice — the snow line dissolves into patches of FULL
// snow over grass instead of a translucent 50/50 veil. Mirrored by
// TerrainNoise.cpp snowPatchAt.
float snowPatch01(vec2 worldXz) {
    // Three incommensurate octaves: the BIG one (~55 m) makes whole
    // areas flip early or late — snow tongues overflowing the line
    // downhill, grass bays reaching up — while the fine ones draw the
    // patch detail without a lattice rhythm. Contrast-stretched so
    // the threshold remap keeps real bite.
    float n = 0.5 * wanderValue01(worldXz * (1.0 / 55.0) + 13.0) +
              0.3 * wanderValue01(worldXz * (1.0 / 9.0) + 37.0) +
              0.2 * wanderValue01(worldXz * (1.0 / 3.1) + 91.0);
    return clamp((n - 0.5) * 1.8 + 0.5, 0.0, 1.0);
}

// Scree apron (talus): the slope band just BELOW the rock threshold —
// eroded material gathers at rock feet. Peaks where bedrock is exposed;
// the wander keeps the band organic. Consumed twice: the weight rule
// grows a SAND apron there (which then blends to grass or water through
// the ordinary falloffs), and terrain.frag biases the sand family's hex
// pick toward the dedicated scree layer (16).
float screeFactor(float slope, float rockExposure, float wander) {
    float band = smoothstep(0.07, 0.14, slope + wander * 0.03) *
                 (1.0 - smoothstep(0.16, 0.26, slope));
    return band * (0.35 + 0.65 * rockExposure);
}

// snowLine arrives with the biome offset already applied. rockShift =
// 0.1 * rockiness; beach forces sand whatever the altitude rules say.
// grassPresence is deliberately absent: the albedo blend renormalizes,
// so it only matters to the scatter mirrors.
TerrainWeights terrainWeights(float h, float slope, float wander,
                              float rockExposure, float seaLevel,
                              float snowLine, float rockShift,
                              float sandiness, float beach) {
    TerrainWeights w;
    w.cliff = smoothstep(0.30, 0.55, slope) * rockExposure;
    w.rock = smoothstep(0.18 - rockShift, 0.35 - rockShift, slope) *
             (1.0 - w.cliff);
    // The WEIGHT-level snow is the high handoff only: below it, the
    // whole grass->snow transition is the DEPOSITION overlay in
    // terrain.frag (snow composited over the grass like snow lies on
    // rock — relief pokes through, feathered edges). A weight-level
    // patch threshold cut hard white shapes instead (measured).
    w.snow = smoothstep(snowLine - 20.0, snowLine + 80.0,
                        h + wander * 26.0) *
             (1.0 - smoothstep(0.25, 0.45, slope));
    w.sand = (1.0 - smoothstep(seaLevel + 1.0 + 6.0 * sandiness,
                               seaLevel + 8.0 + 24.0 * sandiness,
                               h + wander * 5.0)) *
             (1.0 - w.rock - w.cliff);
    w.sand = max(w.sand, beach * (1.0 - w.rock - w.cliff));
    // Talus: the rock-foot band turns sandy (snow still buries it up
    // high) — grass stays the remainder, so scree->grass fades free.
    w.sand = max(w.sand, screeFactor(slope, rockExposure, wander) *
                             max(1.0 - w.rock - w.cliff - w.snow, 0.0));
    w.grass = max(1.0 - w.rock - w.snow - w.sand - w.cliff, 0.0);
    return w;
}
