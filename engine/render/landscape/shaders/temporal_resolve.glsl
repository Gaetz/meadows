// Shared temporal accumulation for raymarched half-res media targets
// (ground mist and the volumetric sky clouds). World-space
// reprojection: the WORLD point at the pixel's depth is reprojected into
// last frame's view — correct for world-anchored media under camera
// motion — then EMA-blended with a soft clamp toward the current sample
// (bounds ghosting at disocclusions without eating the jitter variance
// the EMA exists to average). Pure functions; the caller owns the
// history sampler and the temporal uniforms.

// Returns last frame's uv for `worldPos`, or (-1,-1) when off-screen.
vec2 temporalReprojectUv(vec3 worldPos, mat4 prevViewProj) {
    vec4 clip = prevViewProj * vec4(worldPos, 1.0);
    if (clip.w <= 0.0) {
        return vec2(-1.0);
    }
    vec2 uv = (clip.xy / clip.w) * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return vec2(-1.0);
    }
    return uv;
}

// EMA resolve: alpha = fraction of the CURRENT sample (1 = no history).
// The soft clamp pulls history that strays far from the current sample
// back within a tolerance band. `tolScale` widens it: tight (1) where
// ghosting bites (near, moving foreground — sword, grass tips die in a
// couple of frames), wide (2-3) where CONVERGENCE matters (far mist
// whose long march segments make the per-frame sample noisy — a tight
// clamp there lets the flicker straight through the EMA).
vec4 temporalResolve(vec4 current, vec4 history, float alpha,
                     float tolScale) {
    vec4 tolerance = (abs(current) * 0.35 + 0.03) * tolScale;
    history = clamp(history, current - tolerance, current + tolerance);
    return mix(history, current, alpha);
}
