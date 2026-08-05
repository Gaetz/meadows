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
    w /= (w.x + w.y + w.z);
}

int hexVariantOf(ivec2 v) {
    return int(zoneHash(uint(v.x) * 0x9e3779b9u ^
               uint(v.y) * 0x85ebca6bu) & 3u);
}

// Per-FAMILY variant layer at a lattice vertex (SplatTextures.hpp is
// the layer table): grass 4-way (0/5/6/7 — its own hash, the CPU
// scatter mirror depends on it), rock 4-way (1/8/9/10), snow 3-way
// (2/11/12), sand 4-way (3/13/14/15); cliff stays single (strata
// modulation instead). Distinct salts per family so the patchworks
// never correlate.
// `screeBias` (sand family only): the rock-foot talus flips the pick to
// the dedicated scree layer (16) with rising probability — its fringe
// hex-blends with the ordinary sand variants, and the weight rule's
// falloff carries it into grass or water.
float hexFamilyLayer(int family, ivec2 v, float screeBias) {
    if (family == 0) {
        int g = hexVariantOf(v);
        return g == 0 ? 0.0 : float(4 + g);
    }
    if (family >= 4) {
        return float(family);
    }
    uint h = zoneHash(uint(v.x) * (0x68e31da4u + uint(family) * 977u) ^
                      uint(v.y) * (0xb5297a4du + uint(family) * 331u));
    if (family == 1) {
        int g = int(h & 3u);
        return g == 0 ? 1.0 : float(7 + g); // 8..10
    }
    if (family == 2) {
        int g = int(h % 3u);
        return g == 0 ? 2.0 : float(10 + g); // 11..12
    }
    if (float((h >> 8) & 255u) * (1.0 / 255.0) < screeBias) {
        return 16.0; // scree
    }
    int g = int(h & 3u);
    return g == 0 ? 3.0 : float(12 + g); // 13..15
}

vec2 hexOffsetOf(ivec2 v) {
    uint h = zoneHash(uint(v.x) * 0x68e31da4u ^ uint(v.y) * 0xb5297a4du);
    return vec2(float(h & 0xffffu), float(h >> 16)) * (1.0 / 65535.0);
}

