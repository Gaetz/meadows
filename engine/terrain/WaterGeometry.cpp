#include "engine/terrain/WaterGeometry.hpp"

#include <cmath>

#include <glm/glm.hpp>

#include "engine/terrain/RiverGeometry.hpp"

namespace render::terrain {

namespace {

u32 pushVertex(vector<f32>& verts, f32 x, f32 y, f32 z, f32 flowX,
               f32 flowZ, f32 halfWidth, f32 lateral, f32 arc, f32 endDist,
               f32 material) {
    verts.push_back(x);
    verts.push_back(y);
    verts.push_back(z);
    verts.push_back(flowX);
    verts.push_back(flowZ);
    verts.push_back(halfWidth);
    verts.push_back(lateral);
    verts.push_back(arc);
    verts.push_back(endDist);
    verts.push_back(material);
    return static_cast<u32>(verts.size() / kWaterVertexFloats - 1);
}

// Flat quad at `level`; `foamGate` rides the lateral lane.
void pushQuad(vector<f32>& verts, vector<u32>& indices, f32 x0, f32 z0,
              f32 x1, f32 z1, f32 level, f32 material, f32 foamGate) {
    const u32 v0 = pushVertex(verts, x0, level, z0, 0.0f, 0.0f, 0.0f,
                              foamGate, 0.0f, 1.0e6f, material);
    const u32 v1 = pushVertex(verts, x1, level, z0, 0.0f, 0.0f, 0.0f,
                              foamGate, 0.0f, 1.0e6f, material);
    const u32 v2 = pushVertex(verts, x1, level, z1, 0.0f, 0.0f, 0.0f,
                              foamGate, 0.0f, 1.0e6f, material);
    const u32 v3 = pushVertex(verts, x0, level, z1, 0.0f, 0.0f, 0.0f,
                              foamGate, 0.0f, 1.0e6f, material);
    for (const u32 i : { v0, v2, v1, v0, v3, v2 }) {
        indices.push_back(i);
    }
}

f32 materialOf(u32 index) {
    return static_cast<f32>(glm::min(index, kMaxWaterMaterials - 1));
}

// Shore foam only on lakes >= this wet area — below it the ring
// dominates the surface instead of trimming it.
constexpr f32 kFoamLakeArea = 5000.0f; // m²

// Still-water vertex (far sheets): zero flow, no river uv.
u32 pushStillVertex(vector<f32>& verts, f32 x, f32 y, f32 z) {
    verts.insert(verts.end(), { x, y, z, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                1.0e6f, 0.0f });
    return static_cast<u32>(verts.size() / kWaterVertexFloats - 1);
}

void meshFarLake(const FarWaterSet::Lake& lake, vector<f32>& verts,
                 vector<u32>& indices) {
    // One quad per row RUN of covered coarse cells.
    for (u32 r = 0; r < lake.h; ++r) {
        u32 c = 0;
        while (c < lake.w) {
            if (lake.mask[static_cast<size_t>(r) * lake.w + c] == 0) {
                ++c;
                continue;
            }
            const u32 c0 = c;
            while (c < lake.w &&
                   lake.mask[static_cast<size_t>(r) * lake.w + c] != 0) {
                ++c;
            }
            const f32 x0 = lake.minX + static_cast<f32>(c0) * lake.cell;
            const f32 x1 = lake.minX + static_cast<f32>(c) * lake.cell;
            const f32 z0 = lake.minZ + static_cast<f32>(r) * lake.cell;
            const f32 z1 = z0 + lake.cell;
            const u32 v0 = pushStillVertex(verts, x0, lake.level, z0);
            const u32 v1 = pushStillVertex(verts, x1, lake.level, z0);
            const u32 v2 = pushStillVertex(verts, x1, lake.level, z1);
            const u32 v3 = pushStillVertex(verts, x0, lake.level, z1);
            for (const u32 i : { v0, v2, v1, v0, v3, v2 }) {
                indices.push_back(i);
            }
        }
    }
}

void meshFarRibbon(const FarWaterSet::Ribbon& ribbon, vector<f32>& verts,
                   vector<u32>& indices) {
    if (ribbon.nodes.size() < 2) {
        return;
    }
    u32 prevL = 0;
    u32 prevR = 0;
    bool chained = false;
    for (size_t i = 0; i < ribbon.nodes.size(); ++i) {
        const FarWaterSet::Ribbon::Node& node = ribbon.nodes[i];
        const FarWaterSet::Ribbon::Node& behind =
            ribbon.nodes[i > 0 ? i - 1 : 0];
        const FarWaterSet::Ribbon::Node& ahead =
            ribbon.nodes[i + 1 < ribbon.nodes.size() ? i + 1 : i];
        f32 dx = ahead.x - behind.x;
        f32 dz = ahead.z - behind.z;
        const f32 len = std::sqrt(dx * dx + dz * dz);
        if (len < 1.0e-3f) {
            continue;
        }
        dx /= len;
        dz /= len;
        const u32 left =
            pushStillVertex(verts, node.x - dz * node.halfWidth,
                            node.surface, node.z + dx * node.halfWidth);
        const u32 right =
            pushStillVertex(verts, node.x + dz * node.halfWidth,
                            node.surface, node.z - dx * node.halfWidth);
        if (chained) {
            for (const u32 v : { prevL, prevR, left, prevR, right, left }) {
                indices.push_back(v);
            }
        }
        prevL = left;
        prevR = right;
        chained = true;
    }
}

} // namespace

void meshLakeSheet(const LakeSurface& lake, const WaterBodies& bodies,
                   const TerrainParams& params, vector<f32>& verts,
                   vector<u32>& indices) {
    // The surface follows the basin mask (row runs of water cells,
    // grown half a texel so the sheet reaches under the banks — the
    // depth test cuts the exact shoreline). Maskless lakes
    // (hand-authored ponds) stay one bbox quad.
    if (lake.mask.empty() || lake.maskWidth == 0) {
        constexpr f32 kMargin = 6.0f;
        const f32 area = (lake.maxX - lake.minX) * (lake.maxZ - lake.minZ);
        pushQuad(verts, indices, lake.minX - kMargin, lake.minZ - kMargin,
                 lake.maxX + kMargin, lake.maxZ + kMargin, lake.level,
                 materialOf(lake.materialIndex),
                 area >= kFoamLakeArea ? 1.0f : 0.0f);
        return;
    }
    u32 wet = 0;
    for (const u8 m : lake.mask) {
        wet += m ? 1 : 0;
    }
    const f32 foamGate =
        static_cast<f32>(wet) * lake.maskTexel * lake.maskTexel >=
                kFoamLakeArea
            ? 1.0f
            : 0.0f;
    // Welded sheet with a SMOOTHED boundary ring: the per-texel quads
    // with per-side growth drew the mask's 8-16 m staircase outline
    // straight into the water — crenellated, "blocky" sheets. From
    // Dust's round blobs are a RENDERING property of a heightfield, not
    // a sim method: corners are welded on the lattice, the boundary
    // ring is Laplacian-rounded, and the old per-side semantics (grow
    // under banks so the depth test cuts the true shoreline, flush +
    // skirt at crests, shelf reach, sibling-piece seams) become
    // per-boundary-corner probes along the outward normal.
    const f32 grow = lake.maskTexel * 0.75f;
    const f32 mt = lake.maskTexel;
    // Mask-index predicate (strict: outside the mask is dry). Not the
    // world-space LakeSurface::covers, which rounds to the nearest
    // texel and clamps into the mask.
    const auto covered = [&](i32 mc, i32 mr) {
        return mc >= 0 && mr >= 0 &&
               mc < static_cast<i32>(lake.maskWidth) &&
               mr < static_cast<i32>(lake.maskHeight) &&
               lake.mask[static_cast<size_t>(mr) * lake.maskWidth +
                         static_cast<size_t>(mc)] != 0;
    };
    const i32 mw = static_cast<i32>(lake.maskWidth);
    const i32 mh = static_cast<i32>(lake.maskHeight);
    // Corner lattice (mw+1) x (mh+1): texel m spans corners m..m+1,
    // corner c at minX + (c - 0.5) * mt (point-centred masks).
    const i32 cw = mw + 1;
    const i32 ch = mh + 1;
    const auto cid = [&](i32 c, i32 r) {
        return static_cast<size_t>(r) * cw + static_cast<size_t>(c);
    };
    vector<f32> cx(static_cast<size_t>(cw) * ch);
    vector<f32> cz(static_cast<size_t>(cw) * ch);
    vector<u8> onRing(static_cast<size_t>(cw) * ch, 0);
    vector<u8> crestC(static_cast<size_t>(cw) * ch, 0);
    for (i32 r = 0; r < ch; ++r) {
        for (i32 c = 0; c < cw; ++c) {
            cx[cid(c, r)] = lake.minX + (static_cast<f32>(c) - 0.5f) * mt;
            cz[cid(c, r)] = lake.minZ + (static_cast<f32>(r) - 0.5f) * mt;
            i32 wetCorners = 0;
            wetCorners += covered(c - 1, r - 1) ? 1 : 0;
            wetCorners += covered(c, r - 1) ? 1 : 0;
            wetCorners += covered(c - 1, r) ? 1 : 0;
            wetCorners += covered(c, r) ? 1 : 0;
            onRing[cid(c, r)] = (wetCorners > 0 && wetCorners < 4) ? 1 : 0;
        }
    }
    // A corner-grid edge is part of the ring where it separates a wet
    // texel from a dry one.
    const auto hEdgeRing = [&](i32 c, i32 r) {
        return covered(c, r - 1) != covered(c, r);
    };
    const auto vEdgeRing = [&](i32 c, i32 r) {
        return covered(c - 1, r) != covered(c, r);
    };
    // Round the ring: each ring corner with exactly two ring neighbours
    // relaxes toward them (two Laplacian passes turn the axis-aligned
    // staircase into a smooth closed curve); pinch points stay put.
    for (i32 pass = 0; pass < 2; ++pass) {
        vector<f32> nx = cx;
        vector<f32> nz = cz;
        for (i32 r = 0; r < ch; ++r) {
            for (i32 c = 0; c < cw; ++c) {
                if (!onRing[cid(c, r)]) {
                    continue;
                }
                size_t nb[4];
                i32 n = 0;
                if (c + 1 < cw && hEdgeRing(c, r)) {
                    nb[n++] = cid(c + 1, r);
                }
                if (c > 0 && hEdgeRing(c - 1, r)) {
                    nb[n++] = cid(c - 1, r);
                }
                if (r + 1 < ch && vEdgeRing(c, r)) {
                    nb[n++] = cid(c, r + 1);
                }
                if (r > 0 && vEdgeRing(c, r - 1)) {
                    nb[n++] = cid(c, r - 1);
                }
                if (n != 2) {
                    continue;
                }
                nx[cid(c, r)] = 0.5f * cx[cid(c, r)] +
                                0.25f * (cx[nb[0]] + cx[nb[1]]);
                nz[cid(c, r)] = 0.5f * cz[cid(c, r)] +
                                0.25f * (cz[nb[0]] + cz[nb[1]]);
            }
        }
        cx.swap(nx);
        cz.swap(nz);
    }
    // Per-ring-corner shoreline semantics, probed along the outward
    // normal (away from the adjacent wet texel centres): a bank that
    // HOLDS the level grows the sheet under it, a moderate dip that
    // rises back within reach is a submerged shelf (reach to it), a
    // deep void is the crest (flush, the skirt pours), and a deep point
    // covered by a sibling piece of the same lake is a tile seam (grown
    // overlap).
    for (i32 r = 0; r < ch; ++r) {
        for (i32 c = 0; c < cw; ++c) {
            if (!onRing[cid(c, r)]) {
                continue;
            }
            f32 wx = 0.0f;
            f32 wz = 0.0f;
            i32 wn = 0;
            const i32 tc[4][2] = {
                { c - 1, r - 1 }, { c, r - 1 }, { c - 1, r }, { c, r }
            };
            for (const auto& t : tc) {
                if (covered(t[0], t[1])) {
                    wx += lake.minX + static_cast<f32>(t[0]) * mt;
                    wz += lake.minZ + static_cast<f32>(t[1]) * mt;
                    ++wn;
                }
            }
            if (wn == 0) {
                continue;
            }
            f32 ox = cx[cid(c, r)] - wx / static_cast<f32>(wn);
            f32 oz = cz[cid(c, r)] - wz / static_cast<f32>(wn);
            const f32 ol = std::sqrt(ox * ox + oz * oz);
            if (ol < 1.0e-3f) {
                continue;
            }
            ox /= ol;
            oz /= ol;
            f32 push = grow; // bank by default
            bool isCrest = false;
            bool dipped = false;
            for (i32 s = 0; s < 3; ++s) {
                const f32 d = (0.5f + static_cast<f32>(s)) * mt;
                const f32 px = cx[cid(c, r)] + ox * d;
                const f32 pz = cz[cid(c, r)] + oz * d;
                const f32 g = height(params, px, pz);
                if (g >= lake.level - 2.0f) {
                    if (dipped) {
                        push = d; // shelf: reach the rising bank
                    }
                    break;
                }
                bool seam = false;
                for (const LakeSurface& other : bodies.lakes) {
                    if (&other == &lake) {
                        continue;
                    }
                    if (glm::abs(other.level - lake.level) < 1.0f &&
                        other.covers(px, pz)) {
                        seam = true;
                        break;
                    }
                }
                if (seam) {
                    continue;
                }
                if (g >= lake.level - 6.0f) {
                    dipped = true;
                    continue;
                }
                push = 0.0f;
                isCrest = true;
                break;
            }
            if (!isCrest && dipped && push == grow) {
                // The dip never rose back within reach: conservative
                // crest.
                push = 0.0f;
                isCrest = true;
            }
            cx[cid(c, r)] += ox * push;
            cz[cid(c, r)] += oz * push;
            crestC[cid(c, r)] = isCrest ? 1 : 0;
        }
    }
    // Tops: one quad per covered texel on the welded (rounded, pushed)
    // corners.
    const f32 mat = materialOf(lake.materialIndex);
    vector<u32> cvert(static_cast<size_t>(cw) * ch, ~0u);
    const auto cornerVert = [&](i32 c, i32 r) {
        u32& slot = cvert[cid(c, r)];
        if (slot == ~0u) {
            slot = pushVertex(verts, cx[cid(c, r)], lake.level,
                              cz[cid(c, r)], 0.0f, 0.0f, 0.0f, foamGate,
                              0.0f, 1.0e6f, mat);
        }
        return slot;
    };
    for (i32 r = 0; r < mh; ++r) {
        for (i32 c = 0; c < mw; ++c) {
            if (!covered(c, r)) {
                continue;
            }
            const u32 v00 = cornerVert(c, r);
            const u32 v10 = cornerVert(c + 1, r);
            const u32 v11 = cornerVert(c + 1, r + 1);
            const u32 v01 = cornerVert(c, r + 1);
            for (const u32 k : { v00, v11, v10, v00, v01, v11 }) {
                indices.push_back(k);
            }
        }
    }
    // Crest skirts: the pouring edge, one quad dipping outward from
    // every ring edge whose BOTH corners are crests.
    constexpr f32 kSkirtOut = 2.0f;  // m outward
    constexpr f32 kSkirtDrop = 2.5f; // m down
    const auto skirt = [&](i32 c0, i32 r0, i32 c1, i32 r1, f32 ox,
                           f32 oz) {
        const u32 v0 = cornerVert(c0, r0);
        const u32 v1 = cornerVert(c1, r1);
        const u32 v2 = pushVertex(verts, cx[cid(c1, r1)] + ox,
                                  lake.level - kSkirtDrop,
                                  cz[cid(c1, r1)] + oz, 0.0f, 0.0f, 0.0f,
                                  foamGate, 0.0f, 1.0e6f, mat);
        const u32 v3 = pushVertex(verts, cx[cid(c0, r0)] + ox,
                                  lake.level - kSkirtDrop,
                                  cz[cid(c0, r0)] + oz, 0.0f, 0.0f, 0.0f,
                                  foamGate, 0.0f, 1.0e6f, mat);
        for (const u32 k : { v0, v2, v1, v0, v3, v2 }) {
            indices.push_back(k);
        }
    };
    for (i32 r = 0; r < ch; ++r) {
        for (i32 c = 0; c < cw; ++c) {
            // Outward = toward the DRY side of the edge.
            if (c + 1 < cw && hEdgeRing(c, r) && crestC[cid(c, r)] &&
                crestC[cid(c + 1, r)]) {
                const f32 oz2 = covered(c, r) ? -1.0f : 1.0f;
                skirt(c, r, c + 1, r, 0.0f, oz2 * kSkirtOut);
            }
            if (r + 1 < ch && vEdgeRing(c, r) && crestC[cid(c, r)] &&
                crestC[cid(c, r + 1)]) {
                const f32 ox2 = covered(c, r) ? -1.0f : 1.0f;
                skirt(c, r, c, r + 1, ox2 * kSkirtOut, 0.0f);
            }
        }
    }
}

void meshRiverRibbon(const RiverSurface& river, vector<f32>& verts,
                     vector<u32>& indices) {
    // Ribbon strip along the polyline at the water surface, slightly
    // wider than the carved bed so banks clip it. Tight bends are
    // subdivided by curvature (RiverGeometry) — the water-info raster
    // samples the same conditioned curve.
    vector<RiverNode> nodes = subdivideRiverNodes(river.nodes);
    // No runtime grounding of the chord: the published surfaces are
    // seated on the fine pre-carve ground at bake (the fine re-leveling
    // pass in extractMapHydrology) and the carve digs the bed FROM
    // them, so the bed can never poke above the chord. A resample-and-
    // lift re-raised buried courses into sheets floating over the banks.
    u32 prevL = 0;
    u32 prevR = 0;
    f32 totalArc = 0.0f;
    for (size_t i = 1; i < nodes.size(); ++i) {
        totalArc += std::hypot(nodes[i].x - nodes[i - 1].x,
                               nodes[i].z - nodes[i - 1].z);
    }
    f32 arc = 0.0f;
    const f32 material = materialOf(river.materialIndex);
    for (size_t i = 0; i < nodes.size(); ++i) {
        const RiverNode& node = nodes[i];
        const RiverNode& ahead = nodes[glm::min(i + 1, nodes.size() - 1)];
        const RiverNode& behind = nodes[i > 0 ? i - 1 : 0];
        if (i > 0) {
            arc += std::hypot(node.x - nodes[i - 1].x,
                              node.z - nodes[i - 1].z);
        }
        f32 dx = ahead.x - behind.x;
        f32 dz = ahead.z - behind.z;
        const f32 len = std::sqrt(dx * dx + dz * dz);
        if (len > 1.0e-3f) {
            dx /= len;
            dz /= len;
        } else {
            dx = 1.0f;
            dz = 0.0f;
        }
        // 1.4: the carve wets the inner bank shoulder past the law
        // width — the quad must reach it, the depth test cuts the
        // exact shoreline (dry-bed margins measured at ~1.5x the law
        // width in pierced-band troughs).
        const f32 half = node.halfWidth * 1.4f;
        const f32 flowX = dx * river.flowSpeed;
        const f32 flowZ = dz * river.flowSpeed;
        const u32 left =
            pushVertex(verts, node.x - dz * half, node.surface,
                       node.z + dx * half, flowX, flowZ, node.halfWidth,
                       -1.0f, arc, totalArc - arc, material);
        const u32 right =
            pushVertex(verts, node.x + dz * half, node.surface,
                       node.z - dx * half, flowX, flowZ, node.halfWidth,
                       1.0f, arc, totalArc - arc, material);
        if (i > 0) {
            for (const u32 v : { prevL, prevR, left, prevR, right, left }) {
                indices.push_back(v);
            }
        }
        prevL = left;
        prevR = right;
    }
}

void buildLocalWaterGeometry(const WaterBodies& bodies,
                             const TerrainParams& params,
                             vector<f32>& verts, vector<u32>& indices) {
    for (const LakeSurface& lake : bodies.lakes) {
        meshLakeSheet(lake, bodies, params, verts, indices);
    }
    for (const RiverSurface& river : bodies.rivers) {
        meshRiverRibbon(river, verts, indices);
    }
}

void buildFarWaterGeometry(const FarWaterSet& set, vector<f32>& verts,
                           vector<u32>& indices) {
    for (const FarWaterSet::Lake& lake : set.lakes) {
        meshFarLake(lake, verts, indices);
    }
    for (const FarWaterSet::Ribbon& ribbon : set.ribbons) {
        meshFarRibbon(ribbon, verts, indices);
    }
}

} // namespace render::terrain
