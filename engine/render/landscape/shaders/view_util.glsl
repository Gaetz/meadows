// Shared view-ray helpers for the screen-space passes (requires
// common.glsl).

// World position from a depth-buffer sample. 0..1 clip: the stored depth
// IS ndc z (no *2-1 remap).
vec3 worldFromDepth(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 world = uInvViewProj * ndc;
    return world.xyz / world.w;
}

// Interleaved Gradient Noise (Jimenez): structured screen-space dither
// that filters out smoothly — white noise reads as ink blotches.
float ignJitter(vec2 pixel) {
    return fract(52.9829189 *
                 fract(0.06711056 * pixel.x + 0.00583715 * pixel.y));
}

// Golden-ratio-rolled IGN for the temporal EMA passes: the pattern
// decorrelates frame to frame so the history integrates what a single
// frame under-samples. NOT for the froxels — a phase-rolled spatial
// pattern is not true decorrelation (see froxel_inject.comp).
float ignJitterRolled(vec2 pixel, float frame) {
    return fract(52.9829189 *
                     fract(0.06711056 * pixel.x + 0.00583715 * pixel.y) +
                 frame * 0.61803398875);
}
