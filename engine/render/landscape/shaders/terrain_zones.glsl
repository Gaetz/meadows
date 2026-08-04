// Grass-family ground variants — the "four color" zoning
// (docs/GRASS-REDO.md): the semantic grass layer is subdivided into 4
// material variants laid out as jittered-grid Voronoi cells whose base
// coloring is the 2x2 pattern {0,1;2,3} — two orthogonal neighbors NEVER
// share a variant (the four-color feel without solving a graph), the
// jitter makes the borders organic. Mirrored bit-for-bit by
// TerrainNoise.cpp grassZoneAt (species/clutter coupling) — same hash
// (murmur finalizer), same lattice, same jitter.

const float kGrassZoneSize = 3.0; // meters per zone cell
const float kGrassZoneBorder = 0.8; // blend band across a zone edge (m)

uint zoneHash(uint v) {
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}

vec2 zoneSite(int gx, int gz) {
    uint h = zoneHash(uint(gx) * 0x9e3779b9u ^ uint(gz) * 0x85ebca6bu ^
                      0x5bd1e995u);
    float jx = float(h & 0xffffu) * (1.0 / 65535.0);
    float jz = float(h >> 16) * (1.0 / 65535.0);
    return vec2((float(gx) + 0.2 + 0.6 * jx) * kGrassZoneSize,
                (float(gz) + 0.2 + 0.6 * jz) * kGrassZoneSize);
}

int zoneVariantOf(int gx, int gz) {
    // The guaranteed 2x2 coloring: (x&1) + 2*(z&1).
    return (gx & 1) + 2 * (gz & 1);
}

// Nearest + second-nearest zone of the world position: variantA/B and the
// A weight (1 = pure A; < 1 only inside the border band).
void grassZone(vec2 p, out int variantA, out int variantB,
               out float blendA) {
    int bx = int(floor(p.x / kGrassZoneSize));
    int bz = int(floor(p.y / kGrassZoneSize));
    float d1 = 1e9;
    float d2 = 1e9;
    int c1x = bx;
    int c1z = bz;
    int c2x = bx;
    int c2z = bz;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 site = zoneSite(bx + dx, bz + dz);
            float d = dot(site - p, site - p);
            if (d < d1) {
                d2 = d1;
                c2x = c1x;
                c2z = c1z;
                d1 = d;
                c1x = bx + dx;
                c1z = bz + dz;
            } else if (d < d2) {
                d2 = d;
                c2x = bx + dx;
                c2z = bz + dz;
            }
        }
    }
    variantA = zoneVariantOf(c1x, c1z);
    variantB = zoneVariantOf(c2x, c2z);
    // Edge distance approximation: half the difference of distances to
    // the two nearest sites (exact on the bisector).
    float edge = 0.5 * (sqrt(d2) - sqrt(d1));
    blendA = clamp(0.5 + 0.5 * edge / kGrassZoneBorder, 0.5, 1.0);
}
