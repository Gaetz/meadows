// Grass-family ground variants (docs/GRASS-REDO.md): the semantic grass
// layer is subdivided into 4 material variants by STOCHASTIC HEX-TILING
// (below) — the discrete Voronoi zoning it replaces lived here before
// (git history) and showed its borders. Mirrored by TerrainNoise.cpp
// grassZoneAt (species/clutter coupling) — same hash (murmur
// finalizer), same lattice, same sharpening.

const float kGrassZoneSize = 3.0; // meters per lattice cell

uint zoneHash(uint v) {
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}

// --- Hex-tiling (Heitz-Neyret 2018 / Mikkelsen 2022) -----------------------
// The stochastic upgrade over the discrete Voronoi zones: every point
// blends the THREE vertices of a triangle lattice; each vertex picks a
// grass variant AND a random uv offset (anti-tiling for free). No
// discrete zone border exists anywhere — the barycentric weights,
// sharpened, confine blending to thin organic seams. Mirrored by
// TerrainNoise.cpp grassZoneAt (dominant vertex) for the scatter biases.

const float kHexSharpness = 6.0; // barycentric exponent (seam crispness)

void hexGrass(vec2 p, out ivec2 v0, out ivec2 v1, out ivec2 v2,
              out vec3 w) {
    vec2 q = p / kGrassZoneSize;
    // Skew the plane into the triangle grid (two triangles per cell).
    vec2 s = vec2(q.x - q.y * 0.57735027, q.y * 1.15470054);
    ivec2 base = ivec2(floor(s));
    vec2 f = s - vec2(base);
    if (f.x + f.y < 1.0) {
        v0 = base;
        v1 = base + ivec2(1, 0);
        v2 = base + ivec2(0, 1);
        w = vec3(1.0 - f.x - f.y, f.x, f.y);
    } else {
        v0 = base + ivec2(1, 1);
        v1 = base + ivec2(0, 1);
        v2 = base + ivec2(1, 0);
        w = vec3(f.x + f.y - 1.0, 1.0 - f.x, 1.0 - f.y);
    }
    w = pow(w, vec3(kHexSharpness));
    // 2-tap variant: the smallest barycentric
    // weight is dropped and the pair renormalized — one fewer array
    // fetch per family, the pow-sharpened seams intact. The CPU mirror
    // (grassZoneAt) only reads the DOMINANT vertex: unaffected.
    float wMin = min(w.x, min(w.y, w.z));
    vec3 keep = step(wMin * 1.0001 + 1.0e-8, w);
    if (keep.x + keep.y + keep.z >= 1.5) {
        w *= keep; // ties keep all three (degenerate centers)
    }
    w /= (w.x + w.y + w.z);
}

int hexVariantOf(ivec2 v) {
    return int(zoneHash(uint(v.x) * 0x9e3779b9u ^
               uint(v.y) * 0x85ebca6bu) & 3u);
}

// Per-FAMILY variant layer at a lattice vertex (SplatTextures.hpp is
// the layer table): grass 4-way (0/5/6/7 — its own hash, the CPU
// scatter mirror depends on it), rock 5-way (1/8/9/10/19), snow 4-way
// (2/11/12/20), sand 4-way (3/13/14/15); cliff 3-way in 24 m PANELS
// (4/17/18 — per-hex-cell picks would patchwork a continuous wall).
// Distinct salts per family so the patchworks never correlate.
// `bias` = the sand family's scree flip to layer 16 (rock-foot talus),
// and the GRASS family's subalpine flip to the heath layers 22/23 (the
// large-scale autotile: whole cells adopt the transition ground as the
// snow line nears; the low-contrast heath tolerates the per-vertex flip
// that the white frost could not). The frost-grass layer (21) stays a
// per-pixel DEPOSIT in terrain.frag — a binary cell decision at high
// contrast paints the hex lattice (measured in game).
float hexFamilyLayer(int family, ivec2 v, float bias) {
    if (family == 0) {
        uint hg = zoneHash(uint(v.x) * 0x9e3779b9u ^
                           uint(v.y) * 0x85ebca6bu);
        if (float((hg >> 8) & 255u) * (1.0 / 255.0) < bias) {
            return ((hg >> 16) & 1u) == 0u ? 22.0 : 23.0;
        }
        int g = int(hg & 3u);
        return g == 0 ? 0.0 : float(4 + g);
    }
    if (family >= 4) {
        ivec2 vc = ivec2(floor(vec2(v) / 8.0)); // ~24 m panels
        uint hc = zoneHash(uint(vc.x) * 0x9e3779b9u ^
                           uint(vc.y) * 0x85ebca6bu);
        int g = int(hc % 3u);
        return g == 0 ? 4.0 : float(16 + g); // 17..18
    }
    uint h = zoneHash(uint(v.x) * (0x68e31da4u + uint(family) * 977u) ^
                      uint(v.y) * (0xb5297a4du + uint(family) * 331u));
    if (family == 1) {
        int g = int(h % 5u);
        return g == 0 ? 1.0 : (g == 4 ? 19.0 : float(7 + g)); // 8..10, 19
    }
    if (family == 2) {
        int g = int(h & 3u);
        return g == 0 ? 2.0 : (g == 3 ? 20.0 : float(10 + g)); // 11..12, 20
    }
    if (float((h >> 8) & 255u) * (1.0 / 255.0) < bias) {
        return 16.0; // scree
    }
    int g = int(h & 3u);
    return g == 0 ? 3.0 : float(12 + g); // 13..15
}

vec2 hexOffsetOf(ivec2 v) {
    uint h = zoneHash(uint(v.x) * 0x68e31da4u ^ uint(v.y) * 0xb5297a4du);
    return vec2(float(h & 0xffffu), float(h >> 16)) * (1.0 / 65535.0);
}

