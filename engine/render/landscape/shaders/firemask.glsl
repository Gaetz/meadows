// The fire field's mask (FireScorchMap): one texel per 2 m cell of the
// fire window — R = scorch (0 untouched .. 1 charred), G = ember glow,
// B = the canopy burnt at that cell (a tree's foliage gone, 0..1); cell
// (col, row) is centered on origin + (col, row) * texel.
// uFireScorchInfo = {originX, originZ, 1/texel, n} — n = 0 while nothing
// burnt. Unit 10, bound at slot 8 for the main and mirror passes.
layout(binding = 10) uniform sampler2D uFireScorch;

vec3 fireMaskAt(vec2 xz) {
    if (uFireScorchInfo.w < 0.5) {
        return vec3(0.0);
    }
    vec2 uv = ((xz - uFireScorchInfo.xy) * uFireScorchInfo.z + 0.5) /
              uFireScorchInfo.w;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return vec3(0.0);
    }
    // Explicit lod: this is sampled from VERTEX stages too (tree.vert's
    // leaf fall, grass.vert's shrink), where an implicit-lod fetch has no
    // derivatives — Vulkan reads zero there.
    return textureLod(uFireScorch, uv, 0.0).rgb;
}

// The canopy channel at the CELL a point falls in, unfiltered: a tree's
// burn is stamped on one texel (its base's cell) — the bilinear read of
// fireMaskAt would dilute it with the untouched neighbours.
float fireCanopyAt(vec2 xz) {
    if (uFireScorchInfo.w < 0.5) {
        return 0.0;
    }
    ivec2 cell = ivec2(floor((xz - uFireScorchInfo.xy) * uFireScorchInfo.z + 0.5));
    int n = int(uFireScorchInfo.w + 0.5);
    if (cell.x < 0 || cell.y < 0 || cell.x >= n || cell.y >= n) {
        return 0.0;
    }
    return texelFetch(uFireScorch, cell, 0).b;
}
