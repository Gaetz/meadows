#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "engine/terrain/SandboxTerrain.hpp"
#include "engine/terrain/WaterBodies.hpp"
#include "engine/terrain/generation/TileBake.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/render/landscape/VegetationSystem.hpp"

// Hidden diagnostic (run explicitly): reconstructs the seed-1337 sandbox
// spawn exactly like setSandboxMode does, bakes its tile, and reports
// what actually sits there — the headless twin of "start the game and
// look around the spawn".
//   meadows-tests '-tc=spawn diagnostic*' -ns

using namespace render::terraingen;

TEST_CASE("spawn diagnostic" * doctest::skip()) {
    TileBakeParams params;
    params.worldSeed = 1337;
    ProceduralControlParams controlParams = params.controls;
    controlParams.seed = params.worldSeed;
    const ProceduralControls controls { controlParams };

    // The setSandboxMode probe, verbatim.
    Vec3 start { 2600.0f, 0.0f, 0.0f };
    bool found = false;
    const f32 seaLevel = params.macro.seaLevel;
    for (f32 radius = 2600.0f; radius <= 24000.0f && !found;
         radius += 700.0f) {
        for (u32 step = 0; step < 16 && !found; ++step) {
            const f32 angle =
                radius * 0.0137f + static_cast<f32>(step) * 0.3927f;
            const f32 x = std::cos(angle) * radius;
            const f32 z = std::sin(angle) * radius;
            const f32 h =
                macroHeightAnalytic(controls, params.macro, x, z);
            if (h > seaLevel + 8.0f && h < 95.0f) {
                start = { x, h, z };
                found = true;
            }
        }
    }
    MESSAGE("probe: found=", found, " at (", start.x, ", ", start.z,
            "), analytic h=", start.y, ", sea=", seaLevel);

    const i32 tx = static_cast<i32>(std::floor(start.x / params.tileSize));
    const i32 tz = static_cast<i32>(std::floor(start.z / params.tileSize));
    MESSAGE("spawn tile: (", tx, ", ", tz, ")");
    const TileBakeResult baked = bakeTile(params, tx, tz);

    // Baked ground at the spawn.
    render::TerrainParams tp;
    auto base = std::make_shared<render::TerrainBase>();
    base->regions.push_back(baked.region);
    tp.base = base;
    auto sandbox = std::make_shared<render::SandboxTerrain>();
    sandbox->controls = controlParams;
    sandbox->macro = params.macro;
    tp.sandbox = sandbox;
    const f32 bakedH = render::terrain::height(tp, start.x, start.z);
    MESSAGE("baked h at spawn = ", bakedH, " (analytic said ", start.y,
            ", delta ", bakedH - start.y, ")");
    if (bakedH < seaLevel) {
        MESSAGE("  -> UNDER THE SEA on the baked terrain");
    }

    // Water bodies over/near the spawn.
    u32 covering = 0;
    for (const Lake& lake : baked.lakes) {
        const bool inBbox = start.x >= lake.minX && start.x <= lake.maxX &&
                            start.z >= lake.minZ && start.z <= lake.maxZ;
        const f32 cx = (lake.minX + lake.maxX) * 0.5f;
        const f32 cz = (lake.minZ + lake.maxZ) * 0.5f;
        const f32 dist = std::hypot(cx - start.x, cz - start.z);
        if (inBbox || dist < 600.0f) {
            ++covering;
            MESSAGE("lake near spawn: level=", lake.level,
                    " bbox=(", lake.minX, ",", lake.minZ, ")-(",
                    lake.maxX, ",", lake.maxZ, ") dug=",
                    static_cast<int>(lake.dug), " cells=", lake.cells,
                    inBbox ? "  [SPAWN IN BBOX]" : "");
        }
    }
    MESSAGE("lakes on tile: ", baked.lakes.size(), " (", covering,
            " near spawn), rivers: ", baked.rivers.size());

    // The wetAt verdict the warmup uses, and where the spiral would go.
    render::WaterBodies bodies;
    bodies.seaLevel = seaLevel;
    for (const Lake& lake : baked.lakes) {
        render::LakeSurface surface;
        surface.level = lake.level;
        surface.minX = lake.minX;
        surface.minZ = lake.minZ;
        surface.maxX = lake.maxX;
        surface.maxZ = lake.maxZ;
        surface.maskWidth = lake.maskWidth;
        surface.maskHeight = lake.maskHeight;
        surface.maskTexel = lake.maskTexel;
        surface.mask = lake.mask;
        bodies.lakes.push_back(std::move(surface));
    }
    const auto wetAt = [&](f32 x, f32 z) {
        const f32 h = render::terrain::height(tp, x, z);
        if (h < seaLevel + 2.0f) {
            return true;
        }
        return render::terrain::waterSurfaceAt(bodies, x, z, h + 1.0f)
            .has_value();
    };
    const bool spawnWet = wetAt(start.x, start.z);
    MESSAGE("wetAt(spawn) = ", spawnWet);
    if (spawnWet) {
        for (f32 radius = 60.0f; radius <= 1500.0f; radius += 60.0f) {
            bool dry = false;
            for (u32 k = 0; k < 12 && !dry; ++k) {
                const f32 angle =
                    static_cast<f32>(k) * (6.2831853f / 12.0f);
                const f32 x = start.x + std::cos(angle) * radius;
                const f32 z = start.z + std::sin(angle) * radius;
                if (!wetAt(x, z)) {
                    MESSAGE("relocation would land at (", x, ", ", z,
                            "), h=", render::terrain::height(tp, x, z),
                            " (radius ", radius, ")");
                    dry = true;
                }
            }
            if (dry) {
                break;
            }
        }
    }
    CHECK(found);
}

// Second stage: reproduce the GAME's post-reconcile state around the
// relocated spawn and inspect the water column there.
//   meadows-tests '-tc=spawn diagnostic 2' -ns
TEST_CASE("spawn diagnostic 2" * doctest::skip()) {
    TileBakeParams params;
    params.worldSeed = 1337;
    // The game's relocated spawn from the verified run log.
    const f32 px = 2583.0f;
    const f32 pz = 60.0f;

    const TileBakeResult a = bakeTile(params, 0, -1);
    const TileBakeResult b = bakeTile(params, 0, 0);
    render::TerrainParams tp;
    auto base = std::make_shared<render::TerrainBase>();
    base->regions.push_back(a.region);
    base->regions.push_back(b.region);
    tp.base = base;
    auto sandbox = std::make_shared<render::SandboxTerrain>();
    sandbox->controls = params.controls;
    sandbox->controls.seed = params.worldSeed;
    sandbox->macro = params.macro;
    tp.sandbox = sandbox;

    vector<Lake> lakes = a.lakes;
    lakes.insert(lakes.end(), b.lakes.begin(), b.lakes.end());
    // The scene's reconcile, verbatim: clear mask cells whose REAL
    // ground pokes above the lake level.
    for (Lake& lake : lakes) {
        if (lake.mask.empty()) {
            continue;
        }
        u32 kept = 0;
        for (u32 row = 0; row < lake.maskHeight; ++row) {
            for (u32 col = 0; col < lake.maskWidth; ++col) {
                u8& cell =
                    lake.mask[static_cast<size_t>(row) * lake.maskWidth +
                              col];
                if (!cell) {
                    continue;
                }
                const f32 wx =
                    lake.minX + static_cast<f32>(col) * lake.maskTexel;
                const f32 wz =
                    lake.minZ + static_cast<f32>(row) * lake.maskTexel;
                if (render::terrain::height(tp, wx, wz) >
                    lake.level - 0.15f) {
                    cell = 0;
                    continue;
                }
                ++kept;
            }
        }
        lake.cells = kept;
    }

    const f32 ground = render::terrain::height(tp, px, pz);
    MESSAGE("relocated spawn (", px, ", ", pz, "): baked ground = ",
            ground, ", sea = ", params.macro.seaLevel);
    for (const Lake& lake : lakes) {
        if (px < lake.minX || px > lake.maxX || pz < lake.minZ ||
            pz > lake.maxZ) {
            continue;
        }
        // Mask cell under the point + wet cells nearby (the ribbon
        // quads grow runs by 0.75 texel — a wet neighbour renders a
        // sheet overhead even when THIS cell is dry).
        const u32 col = static_cast<u32>(
            glm::clamp((px - lake.minX) / lake.maskTexel + 0.5f, 0.0f,
                       static_cast<f32>(lake.maskWidth - 1)));
        const u32 row = static_cast<u32>(
            glm::clamp((pz - lake.minZ) / lake.maskTexel + 0.5f, 0.0f,
                       static_cast<f32>(lake.maskHeight - 1)));
        const u8 cell = lake.mask.empty()
                            ? 1
                            : lake.mask[static_cast<size_t>(row) *
                                            lake.maskWidth +
                                        col];
        u32 wetNear = 0;
        for (i32 dz = -2; dz <= 2; ++dz) {
            for (i32 dx = -2; dx <= 2; ++dx) {
                const i32 cx = static_cast<i32>(col) + dx;
                const i32 cz = static_cast<i32>(row) + dz;
                if (cx < 0 || cz < 0 ||
                    cx >= static_cast<i32>(lake.maskWidth) ||
                    cz >= static_cast<i32>(lake.maskHeight)) {
                    continue;
                }
                if (lake.mask.empty() ||
                    lake.mask[static_cast<size_t>(cz) * lake.maskWidth +
                              cx]) {
                    ++wetNear;
                }
            }
        }
        MESSAGE("lake level=", lake.level, " cells=", lake.cells,
                ": spawn IN BBOX, mask cell=", static_cast<int>(cell),
                ", wet cells within 2 texels=", wetNear,
                ", ground vs level: ", ground, " vs ", lake.level,
                (ground < lake.level ? "  [GROUND UNDER LEVEL]" : ""));
    }
    CHECK(true);
}

// Regime map: where the seed-1337 world puts its OLD MASSIFS (plateau
// wearing hills) and HILL CHAINS — teleport targets for visual review.
//   meadows-tests '-tc=regime diagnostic' -ns
TEST_CASE("regime diagnostic" * doctest::skip()) {
    TileBakeParams params;
    params.worldSeed = 1337;
    ProceduralControlParams controlParams = params.controls;
    controlParams.seed = params.worldSeed;
    const ProceduralControls controls { controlParams };

    struct Spot {
        f32 x, z, score, h;
    };
    vector<Spot> massifs;
    vector<Spot> chains;
    for (f32 z = -30000.0f; z <= 30000.0f; z += 500.0f) {
        for (f32 x = -30000.0f; x <= 30000.0f; x += 500.0f) {
            const ControlSample s = controls.at(x, z);
            if (s.sea) {
                continue;
            }
            const f32 h =
                macroHeightAnalytic(controls, params.macro, x, z);
            if (s.plateau > 120.0f && s.hillRelief > 40.0f) {
                massifs.push_back({ x, z, s.plateau + s.hillRelief, h });
            } else if (s.hillRelief > 35.0f && s.plateau < 20.0f &&
                       s.uplift < 0.05f) {
                chains.push_back({ x, z, s.hillRelief, h });
            }
        }
    }
    const auto report = [](const char* label, vector<Spot>& spots) {
        std::sort(spots.begin(), spots.end(),
                  [](const Spot& a, const Spot& b) {
                      return a.score > b.score;
                  });
        u32 shown = 0;
        vector<Spot> kept;
        for (const Spot& spot : spots) {
            bool near = false;
            for (const Spot& other : kept) {
                if (std::hypot(spot.x - other.x, spot.z - other.z) <
                    4000.0f) {
                    near = true;
                    break;
                }
            }
            if (near) {
                continue;
            }
            kept.push_back(spot);
            MESSAGE(label, ": (", spot.x, ", ", spot.z, ")  score=",
                    spot.score, "  altitude~", spot.h);
            if (++shown >= 4) {
                break;
            }
        }
        MESSAGE(label, " samples total: ", spots.size());
    };
    report("OLD MASSIF", massifs);
    report("HILL CHAIN", chains);
    CHECK(true);
}

// What does the tallest terrain actually measure? Scans the analytic
// surface + uplift for the strongest summit candidate, bakes that tile,
// and reports the true (post-erosion, post-rounding) peak.
//   meadows-tests '-tc=height diagnostic' -ns
TEST_CASE("height diagnostic" * doctest::skip()) {
    TileBakeParams params;
    params.worldSeed = 1337;
    ProceduralControlParams controlParams = params.controls;
    controlParams.seed = params.worldSeed;
    const ProceduralControls controls { controlParams };

    struct Candidate {
        f32 x, z, score, base;
    };
    vector<Candidate> candidates;
    for (f32 z = -30000.0f; z <= 30000.0f; z += 250.0f) {
        for (f32 x = -30000.0f; x <= 30000.0f; x += 250.0f) {
            const ControlSample s = controls.at(x, z);
            if (s.sea) {
                continue;
            }
            const f32 h =
                macroHeightAnalytic(controls, params.macro, x, z);
            // The orogeny adds on top of the base where uplift fires.
            candidates.push_back({ x, z, h + s.uplift * 600.0f, h });
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.score > b.score;
              });
    u32 baked = 0;
    vector<Candidate> kept;
    for (const Candidate& c : candidates) {
        bool near = false;
        for (const Candidate& other : kept) {
            if (std::hypot(c.x - other.x, c.z - other.z) < 6000.0f) {
                near = true;
                break;
            }
        }
        if (near) {
            continue;
        }
        kept.push_back(c);
        const i32 tx =
            static_cast<i32>(std::floor(c.x / params.tileSize));
        const i32 tz =
            static_cast<i32>(std::floor(c.z / params.tileSize));
        const TileBakeResult result = bakeTile(params, tx, tz);
        f32 peak = -1.0e9f;
        size_t peakIdx = 0;
        for (size_t i = 0; i < result.region.heights.size(); ++i) {
            if (result.region.heights[i] > peak) {
                peak = result.region.heights[i];
                peakIdx = i;
            }
        }
        const u32 w = result.region.width;
        const f32 px =
            result.region.originX +
            static_cast<f32>(peakIdx % w) * result.region.texelSize;
        const f32 pz =
            result.region.originZ +
            static_cast<f32>(peakIdx / w) * result.region.texelSize;
        MESSAGE("candidate (", c.x, ", ", c.z, ") base=", c.base,
                " uplift-score=", c.score, " -> tile (", tx, ", ", tz,
                "): baked peak ", peak, " m at (", px, ", ", pz, ")");
        if (++baked >= 3) {
            break;
        }
    }
    CHECK(true);
}

// Land-type budget: how the land (sea excluded) splits into plains /
// hills+plateaus / mountains, sampled from the control fields.
//   meadows-tests '-tc=proportion diagnostic' -ns
TEST_CASE("proportion diagnostic" * doctest::skip()) {
    TileBakeParams params;
    params.worldSeed = 1337;
    ProceduralControlParams controlParams = params.controls;
    controlParams.seed = params.worldSeed;
    const ProceduralControls controls { controlParams };

    u64 sea = 0, plains = 0, hills = 0, mountains = 0;
    for (f32 z = -60000.0f; z <= 60000.0f; z += 200.0f) {
        for (f32 x = -60000.0f; x <= 60000.0f; x += 200.0f) {
            const ControlSample s = controls.at(x, z);
            if (s.sea) {
                ++sea;
            } else if (s.uplift > 0.35f) {
                ++mountains;
            } else if (s.hillRelief > 25.0f || s.plateau > 80.0f) {
                ++hills;
            } else {
                ++plains;
            }
        }
    }
    const f64 land = static_cast<f64>(plains + hills + mountains);
    MESSAGE("sea ", 100.0 * sea / (land + sea), "% of world; of land: ",
            "plains ", 100.0 * plains / land, "%, hills+plateaus ",
            100.0 * hills / land, "%, mountains ",
            100.0 * mountains / land, "%");
    CHECK(true);
}

// Coastline census: how much of the shore runs in cliff mode, how high
// the rims stand, and where the best existing sea-cliffs are.
//   meadows-tests '-tc=coast diagnostic' -ns
TEST_CASE("coast diagnostic" * doctest::skip()) {
    TileBakeParams params;
    params.worldSeed = 1337;
    ProceduralControlParams controlParams = params.controls;
    controlParams.seed = params.worldSeed;
    const ProceduralControls controls { controlParams };
    const f32 sea = params.macro.seaLevel;

    struct Spot {
        f32 x, z, rim, cliff;
    };
    vector<Spot> shore;
    const f32 step = 200.0f;
    for (f32 z = -60000.0f; z <= 60000.0f; z += step) {
        for (f32 x = -60000.0f; x <= 60000.0f; x += step) {
            const ControlSample s = controls.at(x, z);
            if (s.sea) {
                continue;
            }
            const bool coastal = controls.at(x - step, z).sea ||
                                 controls.at(x + step, z).sea ||
                                 controls.at(x, z - step).sea ||
                                 controls.at(x, z + step).sea;
            if (!coastal) {
                continue;
            }
            // The coastProfile cliff blend, replicated.
            const f32 cliff = glm::max(
                glm::smoothstep(params.macro.cliffTierStart,
                                params.macro.cliffTierEnd, s.tier),
                glm::smoothstep(0.62f, 0.8f, s.hardness));
            // Rim height: the land just inland of the ramp band.
            const f32 rim =
                macroHeightAnalytic(controls, params.macro, x, z) - sea;
            shore.push_back({ x, z, rim, cliff });
        }
    }
    u32 cliffy = 0, tall = 0;
    for (const Spot& s : shore) {
        if (s.cliff > 0.5f) {
            ++cliffy;
            if (s.rim > 40.0f) {
                ++tall;
            }
        }
    }
    MESSAGE("shore samples: ", shore.size(), "; cliff-mode ",
            100.0 * cliffy / shore.size(), "%, of which rim>40m ",
            cliffy ? 100.0 * tall / cliffy : 0.0, "%");
    std::sort(shore.begin(), shore.end(),
              [](const Spot& a, const Spot& b) {
                  return a.rim * a.cliff > b.rim * b.cliff;
              });
    u32 shown = 0;
    vector<Spot> kept;
    for (const Spot& s : shore) {
        bool near = false;
        for (const Spot& other : kept) {
            if (std::hypot(s.x - other.x, s.z - other.z) < 5000.0f) {
                near = true;
                break;
            }
        }
        if (near || s.cliff < 0.5f) {
            continue;
        }
        kept.push_back(s);
        MESSAGE("cliff coast at (", s.x, ", ", s.z, "): rim ", s.rim,
                " m, cliff ", s.cliff);
        if (++shown >= 5) {
            break;
        }
    }
    // Bake the top spot's tile and walk a transect through it: does
    // the rim survive the real erosion?
    if (!kept.empty()) {
        const Spot& top = kept.front();
        const i32 tx =
            static_cast<i32>(std::floor(top.x / params.tileSize));
        const i32 tz =
            static_cast<i32>(std::floor(top.z / params.tileSize));
        const TileBakeResult baked = bakeTile(params, tx, tz);
        const auto h = [&](f32 x, f32 z) {
            const auto& r = baked.region;
            const i32 col = static_cast<i32>(
                std::lround((x - r.originX) / r.texelSize));
            const i32 row = static_cast<i32>(
                std::lround((z - r.originZ) / r.texelSize));
            if (col < 0 || row < 0 || col >= static_cast<i32>(r.width) ||
                row >= static_cast<i32>(r.height)) {
                return -9999.0f;
            }
            return r.heights[static_cast<size_t>(row) * r.width + col];
        };
        // Seaward direction: the neighbour that was sea in the scan.
        f32 dx = 0.0f, dz = 0.0f;
        if (controls.at(top.x - step, top.z).sea) {
            dx = -1.0f;
        } else if (controls.at(top.x + step, top.z).sea) {
            dx = 1.0f;
        } else if (controls.at(top.x, top.z - step).sea) {
            dz = -1.0f;
        } else {
            dz = 1.0f;
        }
        for (f32 d = -600.0f; d <= 600.0f; d += 150.0f) {
            MESSAGE("  transect ", d, " m seaward: baked h = ",
                    h(top.x + dx * d, top.z + dz * d));
        }
    }
    CHECK(true);
}

// Calibration data for the erosion-aware analytic (far silhouettes):
// bakes a mountain tile and a lowland tile, then buckets baked-minus-
// analytic by analytic height-above-sea, with the mean keep fraction.
//   meadows-tests '-tc=erosion calibration' -ns
TEST_CASE("erosion calibration" * doctest::skip()) {
    TileBakeParams params;
    params.worldSeed = 1337;
    ProceduralControlParams controlParams = params.controls;
    controlParams.seed = params.worldSeed;
    const ProceduralControls controls { controlParams };
    const f32 sea = params.macro.seaLevel;

    const auto sampleTile = [&](i32 tx, i32 tz) {
        const TileBakeResult r = bakeTile(params, tx, tz);
        struct Bucket {
            f64 delta { 0.0 };
            f64 keep { 0.0 };
            u32 count { 0 };
        };
        constexpr u32 kBuckets = 12;
        constexpr f32 kBand = 100.0f;
        array<Bucket, kBuckets> buckets {};
        const u32 w = r.region.width;
        for (u32 row = 0; row < r.region.height; row += 8) {
            for (u32 col = 0; col < w; col += 8) {
                const f32 x = r.region.originX +
                              static_cast<f32>(col) * r.region.texelSize;
                const f32 z = r.region.originZ +
                              static_cast<f32>(row) * r.region.texelSize;
                const f32 ha =
                    macroHeightAnalytic(controls, params.macro, x, z);
                if (ha <= sea) {
                    continue;
                }
                const u32 b = glm::min(
                    kBuckets - 1,
                    static_cast<u32>((ha - sea) / kBand));
                const f32 hb =
                    r.region.heights[static_cast<size_t>(row) * w + col];
                const ControlSample s = controls.at(x, z);
                buckets[b].delta += hb - ha;
                buckets[b].keep +=
                    glm::min(0.5f, s.plateau * 0.0008f);
                ++buckets[b].count;
            }
        }
        MESSAGE("tile (", tx, ", ", tz, "):");
        for (u32 b = 0; b < kBuckets; ++b) {
            if (buckets[b].count < 8) {
                continue;
            }
            MESSAGE("  h-sea [", b * 100, ",", (b + 1) * 100,
                    "): mean delta ",
                    buckets[b].delta / buckets[b].count, " keep ",
                    buckets[b].keep / buckets[b].count, " (n=",
                    buckets[b].count, ")");
        }
    };
    sampleTile(-6, 5); // the tallest measured massif
    sampleTile(-1, 0); // the spawn lowlands
    CHECK(true);
}

// Where do the scanned forest-floor debris land around the spawn? Lists
// the fallen trunks (kFirstDebris+1) nearest to the game's spawn so a
// dev can walk to one and validate the scan pipeline in place.
//   meadows-tests '-tc=spawn debris*' -ns
TEST_CASE("spawn debris diagnostic" * doctest::skip()) {
    TileBakeParams params;
    params.worldSeed = 1337;
    // The current probe spawn (run "spawn diagnostic" if this drifts;
    // the game's mist-map log confirms it: center ~(2032, 1608)).
    const f32 px = 2038.28f;
    const f32 pz = 1614.13f;

    const TileBakeResult b = bakeTile(params, 0, 0);
    render::TerrainParams tp;
    auto base = std::make_shared<render::TerrainBase>();
    base->regions.push_back(b.region);
    tp.base = base;
    auto sandbox = std::make_shared<render::SandboxTerrain>();
    sandbox->controls = params.controls;
    sandbox->controls.seed = params.worldSeed;
    sandbox->macro = params.macro;
    tp.sandbox = sandbox;

    struct Hit {
        f32 x, y, z, scale, dist;
        bool trunk;
    };
    vector<Hit> hits;
    u32 denseForestChunks = 0; // >= 25 trees: forest-interior ground
    f32 nearestDense = 1.0e9f;
    const i32 ccx = static_cast<i32>(std::floor(px / 64.0f));
    const i32 ccz = static_cast<i32>(std::floor(pz / 64.0f));
    constexpr i32 kScanRadius = 14; // ~900 m
    for (i32 cz = ccz - kScanRadius; cz <= ccz + kScanRadius; ++cz) {
        for (i32 cx = ccx - kScanRadius; cx <= ccx + kScanRadius; ++cx) {
            const auto buckets = render::scatterProps(tp, cx, cz);
            u32 trees = 0;
            for (u32 v = 0; v < render::VegetationSystem::kTreeVariants;
                 ++v) {
                trees += static_cast<u32>(buckets[v].size());
            }
            if (trees >= 25) {
                ++denseForestChunks;
                const f32 dcx =
                    (static_cast<f32>(cx) + 0.5f) * 64.0f - px;
                const f32 dcz =
                    (static_cast<f32>(cz) + 0.5f) * 64.0f - pz;
                nearestDense =
                    glm::min(nearestDense, std::hypot(dcx, dcz));
            }
            for (u32 v = render::VegetationSystem::kFirstDebris;
                 v < render::VegetationSystem::kVariantCount; ++v) {
                for (const auto& prop : buckets[v]) {
                    const f32 dx = prop.positionScale.x - px;
                    const f32 dz = prop.positionScale.z - pz;
                    hits.push_back(
                        { prop.positionScale.x, prop.positionScale.y,
                          prop.positionScale.z, prop.positionScale.w,
                          std::hypot(dx, dz),
                          v == render::VegetationSystem::kFirstDebris +
                                   1 });
                }
            }
        }
    }
    std::sort(hits.begin(), hits.end(),
              [](const Hit& l, const Hit& r) { return l.dist < r.dist; });
    MESSAGE("debris within ", kScanRadius * 64, " m of spawn (", px, ", ",
            pz, "): ", hits.size());
    u32 trunks = 0;
    for (const Hit& hit : hits) {
        if (!hit.trunk) {
            continue;
        }
        ++trunks;
        if (trunks <= 8) {
            MESSAGE("fallen trunk at (", hit.x, ", ", hit.z,
                    ")  h=", hit.y, "  scale=", hit.scale, "  dist=",
                    hit.dist, " m");
        }
    }
    u32 stumps = 0;
    for (const Hit& hit : hits) {
        if (hit.trunk || ++stumps > 4) {
            continue;
        }
        MESSAGE("stump at (", hit.x, ", ", hit.z, ")  dist=", hit.dist,
                " m");
    }
    MESSAGE("total: ", trunks, " trunk(s), ", hits.size() - trunks,
            " stump(s); dense-forest chunks: ", denseForestChunks,
            " (nearest ", nearestDense, " m)");
    CHECK(true);
}

// How much relief does each erosion stage take? Bakes the tallest
// massif tile with stages toggled off and reports the height stats —
// the answer to "does erosion flatten everything".
//   meadows-tests '-tc=erosion strength*' -ns
TEST_CASE("erosion strength diagnostic" * doctest::skip()) {
    const auto stats = [](const char* label, TileBakeParams params) {
        const TileBakeResult r = bakeTile(params, -6, 5);
        vector<f32> above;
        const f32 sea = params.macro.seaLevel;
        f32 maxH = 0.0f;
        for (u32 row = 0; row < r.region.height; row += 4) {
            for (u32 col = 0; col < r.region.width; col += 4) {
                const f32 h =
                    r.region.heights[static_cast<size_t>(row) *
                                         r.region.width +
                                     col];
                maxH = glm::max(maxH, h);
                if (h > sea) {
                    above.push_back(h - sea);
                }
            }
        }
        std::sort(above.begin(), above.end());
        const auto pct = [&](f32 p) {
            return above.empty()
                       ? 0.0f
                       : above[static_cast<size_t>(
                             p * static_cast<f32>(above.size() - 1))];
        };
        f64 mean = 0.0;
        for (const f32 h : above) {
            mean += h;
        }
        mean /= glm::max<size_t>(above.size(), 1);
        MESSAGE(label, ": max=", maxH, " mean-above-sea=", mean);
        MESSAGE("  p10=", pct(0.10f), " p25=", pct(0.25f),
                " p40=", pct(0.40f), " p50=", pct(0.50f),
                " p60=", pct(0.60f), " p75=", pct(0.75f),
                " p90=", pct(0.90f), " p95=", pct(0.95f),
                " p99=", pct(0.99f));
    };
    TileBakeParams base;
    base.worldSeed = 1337;
    stats("default            ", base);
    TileBakeParams noRound = base;
    noRound.rounding.strength = 0.0f;
    stats("rounding OFF       ", noRound);
    TileBakeParams noErosion = noRound;
    noErosion.fluvial.iterations = 0;
    noErosion.thermal.iterations = 0;
    stats("erosion+rounding OFF", noErosion);
    for (const i32 iterations : { 80, 60, 40 }) {
        TileBakeParams softer = base;
        softer.fluvial.iterations = iterations;
        stats("fluvial reduced     ", softer);
    }
    CHECK(true);
}

// UV health of the scanned rocks (docs/GRASS-REDO.md props): per model,
// the per-triangle texel-stretch distribution (world area vs uv area)
// BEFORE and AFTER decimation — the striped-face hunt ("la texture
// n'en fait pas le tour" on the pale boulder).
//   meadows-tests '-tc=rock uv diagnostic' -ns
#include "engine/assets/GltfMesh.hpp"
#include "engine/assets/MeshSimplify.hpp"
TEST_CASE("rock uv diagnostic" * doctest::skip()) {
    const char* kRocks[] = {
        "game/data/base/models/scans/stone_01/stone_01.gltf",
        "game/data/base/models/scans/rock_moss_set_01/rock_moss_set_01.gltf",
        "game/data/base/models/scans/rock_boulder_dry/rock_boulder_dry.gltf",
        "game/data/base/models/scans/boulder_01/boulder_01.gltf",
    };
    const auto stats = [](const render::MeshData& mesh, const char* tag) {
        u32 degenerate = 0;
        u32 stretched = 0;
        u32 tris = 0;
        f32 uvMinX = 1.0e9f, uvMaxX = -1.0e9f;
        f32 uvMinY = 1.0e9f, uvMaxY = -1.0e9f;
        for (const render::MeshVertex& v : mesh.vertices) {
            uvMinX = glm::min(uvMinX, v.uv.x);
            uvMaxX = glm::max(uvMaxX, v.uv.x);
            uvMinY = glm::min(uvMinY, v.uv.y);
            uvMaxY = glm::max(uvMaxY, v.uv.y);
        }
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const auto& a = mesh.vertices[mesh.indices[i]];
            const auto& b = mesh.vertices[mesh.indices[i + 1]];
            const auto& c = mesh.vertices[mesh.indices[i + 2]];
            const f32 wArea = 0.5f * glm::length(glm::cross(
                b.position - a.position, c.position - a.position));
            const Vec2 e1 = b.uv - a.uv;
            const Vec2 e2 = c.uv - a.uv;
            const f32 uvArea =
                0.5f * std::abs(e1.x * e2.y - e1.y * e2.x);
            if (wArea < 1.0e-8f) {
                continue;
            }
            ++tris;
            const f32 texelDensity = uvArea / wArea;
            if (texelDensity < 1.0e-5f) {
                ++degenerate; // stretched to streaks
            } else if (texelDensity < 1.0e-3f) {
                ++stretched;
            }
        }
        MESSAGE("  ", tag, ": ", tris, " tris, uv x[", uvMinX, ",",
                uvMaxX, "] y[", uvMinY, ",", uvMaxY, "], degenerate ",
                degenerate, ", stretched ", stretched);
    };
    for (const char* path : kRocks) {
        auto mesh = assets::loadGltfMesh(path);
        if (!mesh) {
            MESSAGE(path, ": LOAD FAILED");
            continue;
        }
        MESSAGE(path, ":");
        stats(*mesh, "source");
        render::MeshData simplified = *mesh;
        assets::simplifyMesh(simplified, 700);
        stats(simplified, "decimated 700");
    }
    CHECK(true);
}
