#version 460 core
#include "compat.glsl"
#include "common.glsl"

// Brick 31 — rain streaks, fully procedural (MEADOWS_VERTEX_INDEX, no buffers):
// hash(i) positions in a camera cylinder, scrolled by time (zero CPU
// sim), tilted by the wind, killed under roofs by the top-down
// occlusion depth (the Community-Shaders idea, our bake pattern).

layout(binding = 9) uniform sampler2D uRainOcclusion;

layout(location = 0) out float vAlpha;

float hash11(float p) {
    p = fract(p * 0.1031);
    p *= p + 33.33;
    return fract(p * p * 2.0);
}

void main() {
    const float kRadius = 26.0;
    const float kHeight = 24.0;
    int streak = MEADOWS_VERTEX_INDEX / 6;
    int corner = MEADOWS_VERTEX_INDEX % 6;
    float fi = float(streak);
    float h1 = hash11(fi + 0.13);
    float h2 = hash11(fi + 7.77);
    float h3 = hash11(fi + 41.3);

    vec2 xz = uCameraPos.xz + (vec2(h1, h2) * 2.0 - 1.0) * kRadius;
    float fall = 13.0 + h3 * 7.0;
    float y = uCameraPos.y + kHeight * 0.5 -
              mod(h3 * 977.0 + uTime.x * fall, kHeight);
    vec3 base = vec3(xz.x, y, xz.y);

    // Wind tilt shared with the sway system; streaks lean with gusts.
    vec3 dirDown =
        normalize(vec3(0.18 * uWindInfo.y, -1.0, 0.06 * uWindInfo.y));
    vec3 view = normalize(base - uCameraPos.xyz);
    vec3 right = normalize(cross(view, dirDown)) * 0.016;
    float len = 0.45 + h3 * 0.25;

    // Under-roof kill: the top-down depth says how high the cover above
    // this streak column is; a streak below it never spawns.
    vec4 occ = uRainOcclusionViewProj * vec4(base, 1.0);
    vec2 occUv = occ.xy * 0.5 + 0.5;
    if (all(greaterThan(occUv, vec2(0.0))) &&
        all(lessThan(occUv, vec2(1.0)))) {
        float coverDepth = texture(uRainOcclusion, occUv).r;
        // Ortho: ndc z of the streak vs the first blocker from above.
        if (occ.z * 0.5 + 0.5 > coverDepth + 0.002) {
            gl_Position = vec4(0.0, 0.0, 2.0, 1.0); // clipped away
            vAlpha = 0.0;
            return;
        }
    }

    vec3 offset = (corner == 0 || corner == 3) ? -right
                  : (corner == 2 || corner == 4)
                      ? right + dirDown * len
                      : (corner == 1 ? right : -right + dirDown * len);
    vec3 world = base + offset;

    float radial = length(base.xz - uCameraPos.xz) / kRadius;
    vAlpha = (1.0 - radial * radial) * 0.35 * uStormInfo.y;
    gl_Position = uViewProj * vec4(world, 1.0);
}
