#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>

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
    sampleTile(-7, -3); // the tallest measured massif (adopted world)
    sampleTile(2, 0); // the spawn tile (adopted world)
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
    const f32 px = 8196.77f;
    const f32 pz = 230.072f;

    const TileBakeResult b = bakeTile(params, 2, 0);
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
        const TileBakeResult r = bakeTile(params, -7, -3);
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

// Variety along a walk: bakes two 16 km transects through the spawn
// (E-W and N-S), then scores 250 m windows — the distance a player runs
// in ~45 s (movementSpeed ~110 x 1/20 = 5.5 m/s). A window is an
// "event" when the relief regime flips, water is crossed, or local
// relief exceeds 25 m. Answers "does the landscape change often enough
// while walking".
//   meadows-tests '-tc=variety transect*' -ns
TEST_CASE("variety transect diagnostic" * doctest::skip()) {
    TileBakeParams params;
    params.worldSeed = 1337;
    ProceduralControlParams controlParams = params.controls;
    controlParams.seed = params.worldSeed;
    const ProceduralControls controls { controlParams };
    const f32 seaLevel = params.macro.seaLevel;
    const f32 px = 8196.77f; // the game's confirmed spawn (adopted world)
    const f32 pz = 230.072f;

    struct BakedTiles {
        render::TerrainParams tp;
        render::WaterBodies bodies;
        vector<River> rivers;
    };
    // Bake every tile the two transects touch, share (0, 0).
    std::map<std::pair<i32, i32>, TileBakeResult> tiles;
    const auto tileOf = [&](f32 x, f32 z) {
        return std::make_pair(
            static_cast<i32>(std::floor(x / params.tileSize)),
            static_cast<i32>(std::floor(z / params.tileSize)));
    };
    const f32 kHalf = 8000.0f;
    const f32 kStep = 25.0f;
    for (f32 d = -kHalf; d <= kHalf; d += kStep) {
        for (const auto& key : { tileOf(px + d, pz), tileOf(px, pz + d) }) {
            if (!tiles.count(key)) {
                MESSAGE("baking tile (", key.first, ", ", key.second, ")");
                tiles.emplace(key,
                              bakeTile(params, key.first, key.second));
            }
        }
    }
    BakedTiles world;
    auto base = std::make_shared<render::TerrainBase>();
    world.bodies.seaLevel = seaLevel;
    for (const auto& [key, baked] : tiles) {
        MESSAGE("tile (", key.first, ", ", key.second, "): ",
                baked.lakes.size(), " lakes, ", baked.rivers.size(),
                " rivers");
        base->regions.push_back(baked.region);
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
            world.bodies.lakes.push_back(std::move(surface));
        }
        world.rivers.insert(world.rivers.end(), baked.rivers.begin(),
                            baked.rivers.end());
    }
    world.tp.base = base;
    auto sandbox = std::make_shared<render::SandboxTerrain>();
    sandbox->controls = controlParams;
    sandbox->macro = params.macro;
    world.tp.sandbox = sandbox;

    const auto wetAt = [&](f32 x, f32 z, f32 h) {
        if (h < seaLevel + 0.5f) {
            return true;
        }
        if (render::terrain::waterSurfaceAt(world.bodies, x, z, h + 1.0f)
                .has_value()) {
            return true;
        }
        for (const River& river : world.rivers) {
            for (const RiverPoint& p : river.points) {
                const f32 reach = glm::max(p.halfWidth, 4.0f);
                if (std::abs(p.x - x) < reach &&
                    std::abs(p.z - z) < reach) {
                    return true;
                }
            }
        }
        return false;
    };
    const auto regimeOf = [&](f32 x, f32 z) -> int {
        const ControlSample s = controls.at(x, z);
        if (s.sea) {
            return 0;
        }
        if (s.uplift > 0.35f) {
            return 3;
        }
        if (s.hillRelief > 25.0f || s.plateau > 80.0f) {
            return 2;
        }
        return 1;
    };
    const auto runTransect = [&](const char* label, f32 dirX, f32 dirZ) {
        constexpr u32 kWindow = 10; // 10 x 25 m = 250 m = ~45 s of run
        constexpr f32 kTan10 = 0.1763f;
        constexpr f32 kTan15 = 0.2679f;
        constexpr f32 kTan30 = 0.5774f;
        f32 minH = 1.0e9f, maxH = -1.0e9f;
        f64 meanH = 0.0;
        u32 samples = 0, steep15 = 0, steep30 = 0;
        vector<f32> windowRelief;
        u32 flatWindows = 0, reliefEvents = 0, regimeEvents = 0,
            waterEvents = 0;
        // Terrain-family census of the 250 m windows (the 40/35/25
        // target): socle = gentle and low-relief, drame = wall-steep.
        u32 socleWindows = 0, versantWindows = 0, drameWindows = 0;
        u32 plateauWindows = 0;
        u32 seaWindows = 0;
        // Longest stretch containing a >30° step and no <15° foothold.
        f32 maxImpassable = 0.0f, sinceFoothold = 0.0f;
        bool wallInRun = false;
        vector<u8> eventTypes; // 0 relief, 1 regime, 2 water
        f32 lastEventD = -kHalf;
        f32 worstGap = 0.0f;
        f64 gapSum = 0.0;
        u32 gapCount = 0;
        int prevRegime = -1;
        bool prevWet = false;
        f32 wMin = 1.0e9f, wMax = -1.0e9f;
        u32 inWindow = 0;
        f32 prevH = 0.0f;
        vector<f32> windowSlopes;
        for (f32 d = -kHalf; d <= kHalf; d += kStep) {
            const f32 x = px + dirX * d;
            const f32 z = pz + dirZ * d;
            const f32 h = render::terrain::height(world.tp, x, z);
            minH = glm::min(minH, h);
            maxH = glm::max(maxH, h);
            meanH += h;
            if (samples > 0) {
                const f32 slope = std::abs(h - prevH) / kStep;
                if (slope > kTan30) {
                    ++steep30;
                } else if (slope > kTan15) {
                    ++steep15;
                }
                windowSlopes.push_back(slope);
                if (slope < kTan15) {
                    if (wallInRun) {
                        maxImpassable =
                            glm::max(maxImpassable, sinceFoothold);
                    }
                    sinceFoothold = 0.0f;
                    wallInRun = false;
                } else {
                    sinceFoothold += kStep;
                    wallInRun = wallInRun || slope > kTan30;
                }
            }
            prevH = h;
            ++samples;
            wMin = glm::min(wMin, h);
            wMax = glm::max(wMax, h);
            if (++inWindow < kWindow) {
                continue;
            }
            // One 250 m window closes here. Fully open-water windows
            // leave the census (they would read as flat socle) and
            // pause the event clock: crossing a gulf is its own
            // continuous experience, not landscape monotony.
            if (wMax < seaLevel + 0.5f) {
                ++seaWindows;
                lastEventD = d;
                prevRegime = -1;
                prevWet = true;
                windowSlopes.clear();
                wMin = 1.0e9f;
                wMax = -1.0e9f;
                inWindow = 0;
                continue;
            }
            const f32 relief = wMax - wMin;
            windowRelief.push_back(relief);
            std::sort(windowSlopes.begin(), windowSlopes.end());
            const f32 medianSlope =
                windowSlopes.empty()
                    ? 0.0f
                    : windowSlopes[windowSlopes.size() / 2];
            windowSlopes.clear();
            const f32 cx = px + dirX * (d - 125.0f);
            const f32 cz = pz + dirZ * (d - 125.0f);
            if (medianSlope > kTan30) {
                ++drameWindows;
            } else if (medianSlope < kTan10 && relief < 15.0f) {
                ++socleWindows;
                // The dev likes his plateaus: count the high socles
                // apart so the budget shows them instead of melting
                // them into the plains.
                if (controls.at(cx, cz).plateau > 80.0f) {
                    ++plateauWindows;
                }
            } else {
                ++versantWindows;
            }
            const int regime = regimeOf(cx, cz);
            const bool wet =
                wetAt(cx, cz, render::terrain::height(world.tp, cx, cz));
            bool event = false;
            if (relief < 8.0f) {
                ++flatWindows;
            }
            if (relief > 25.0f) {
                ++reliefEvents;
                eventTypes.push_back(0);
                event = true;
            }
            if (prevRegime >= 0 && regime != prevRegime) {
                ++regimeEvents;
                eventTypes.push_back(1);
                event = true;
            }
            if (wet && !prevWet) {
                ++waterEvents;
                eventTypes.push_back(2);
                event = true;
            }
            prevRegime = regime;
            prevWet = wet;
            if (event) {
                const f32 gap = d - lastEventD;
                worstGap = glm::max(worstGap, gap);
                gapSum += gap;
                ++gapCount;
                lastEventD = d;
            }
            wMin = 1.0e9f;
            wMax = -1.0e9f;
            inWindow = 0;
        }
        worstGap = glm::max(worstGap, kHalf - lastEventD);
        if (wallInRun) {
            maxImpassable = glm::max(maxImpassable, sinceFoothold);
        }
        std::sort(windowRelief.begin(), windowRelief.end());
        const f32 medianRelief =
            windowRelief.empty()
                ? 0.0f
                : windowRelief[windowRelief.size() / 2];
        const u32 windows = static_cast<u32>(windowRelief.size());
        u32 typeCounts[3] = { 0, 0, 0 };
        for (const u8 type : eventTypes) {
            ++typeCounts[type];
        }
        const u32 dominantType =
            glm::max(typeCounts[0], glm::max(typeCounts[1], typeCounts[2]));
        MESSAGE(std::string(label), ": h [", minH, ", ", maxH, "] mean ",
                meanH / samples, " (sea ", seaLevel, ")");
        MESSAGE("  windows(250m)=", windows, " land (", seaWindows,
                " sea)  flat(<8m relief) ",
                100.0f * static_cast<f32>(flatWindows) / windows,
                "%  median relief ", medianRelief, " m");
        MESSAGE("  families: socle ",
                100.0f * static_cast<f32>(socleWindows) / windows,
                "% (dont plateau ",
                100.0f * static_cast<f32>(plateauWindows) / windows,
                "%), versant ",
                100.0f * static_cast<f32>(versantWindows) / windows,
                "%, drame ",
                100.0f * static_cast<f32>(drameWindows) / windows,
                "%  (target 40/35/25, land only)");
        MESSAGE("  events: relief>25m ", reliefEvents, ", regime ",
                regimeEvents, ", water ", waterEvents,
                "  | mean event spacing ",
                gapCount ? gapSum / gapCount : 16000.0, " m, worst gap ",
                worstGap, " m  | dominant type ",
                eventTypes.empty()
                    ? 0.0f
                    : 100.0f * static_cast<f32>(dominantType) /
                          static_cast<f32>(eventTypes.size()),
                "%");
        MESSAGE("  slope: >15° ",
                100.0f * static_cast<f32>(steep15) / samples, "%, >30° ",
                100.0f * static_cast<f32>(steep30) / samples,
                "%  | max impassable stretch ", maxImpassable,
                " m  | water crossings/km ",
                static_cast<f32>(waterEvents) / (2.0f * kHalf / 1000.0f));
    };
    runTransect("E-W", 1.0f, 0.0f);
    runTransect("N-S", 0.0f, 1.0f);
    CHECK(true);
}

// Distant views from TRAVEL POINTS (a deterministic jittered grid of
// walkable spots, not just the spawn): per point, the analytic horizon
// on 72 azimuths to 18 km, plus the two objective layers of the target
// (an alpine summit reachable at ~6 km, a marked hill at ~3 km).
// Acceptance: >= 30/72 open azimuths, a landmark > 2° beyond 3 km,
// both layers present from most points.
//   meadows-tests '-tc=vista diagnostic' -ns
TEST_CASE("vista diagnostic" * doctest::skip()) {
    TileBakeParams params;
    params.worldSeed = 1337;
    ProceduralControlParams controlParams = params.controls;
    controlParams.seed = params.worldSeed;
    const ProceduralControls controls { controlParams };
    const f32 sea = params.macro.seaLevel;
    const auto ha = [&](f32 x, f32 z) {
        return macroHeightAnalytic(controls, params.macro, x, z);
    };
    // Deterministic per-cell hash (splitmix-style; std::hash is not
    // portable across toolchains).
    const auto hash01 = [&](i32 cx, i32 cz, u32 salt) {
        u64 v = (static_cast<u64>(static_cast<u32>(cx)) << 32) ^
                static_cast<u32>(cz) ^ (static_cast<u64>(salt) << 17) ^
                params.worldSeed;
        v ^= v >> 30;
        v *= 0xbf58476d1ce4e5b9ull;
        v ^= v >> 27;
        v *= 0x94d049bb133111ebull;
        v ^= v >> 31;
        return static_cast<f32>(v & 0xffffffu) / 16777215.0f;
    };

    // Travel points: jittered 8 km cells over +/-16 km around the
    // spawn, kept when they land on walkable ground (dry, below the
    // alpine band) — where a player actually journeys.
    struct Travel {
        f32 x, z, h;
    };
    vector<Travel> points;
    for (i32 cz = -2; cz <= 1 && points.size() < 12; ++cz) {
        for (i32 cx = -2; cx <= 1 && points.size() < 12; ++cx) {
            const f32 x = 8196.77f +
                          (static_cast<f32>(cx) + 0.2f +
                           0.6f * hash01(cx, cz, 11)) *
                              8000.0f;
            const f32 z = 230.072f +
                          (static_cast<f32>(cz) + 0.2f +
                           0.6f * hash01(cx, cz, 23)) *
                              8000.0f;
            const f32 h = ha(x, z);
            if (h > sea + 8.0f && h < sea + 450.0f) {
                points.push_back({ x, z, h });
            }
        }
    }
    MESSAGE("travel points kept: ", points.size(), "/16");

    u32 openOk = 0, landmarkOk = 0, summitOk = 0, hillOk = 0;
    for (const Travel& p : points) {
        const f32 eye = p.h + 1.7f;
        u32 openAzimuths = 0;
        bool landmark = false;
        for (u32 a = 0; a < 72; ++a) {
            const f32 azimuth = static_cast<f32>(a) * (6.2831853f / 72.0f);
            const f32 dx = std::cos(azimuth);
            const f32 dz = std::sin(azimuth);
            f32 bestAngle = -90.0f;
            f32 bestDist = 0.0f;
            for (f32 dist = 300.0f; dist <= 18000.0f; dist += 100.0f) {
                const f32 h = ha(p.x + dx * dist, p.z + dz * dist);
                const f32 angle = std::atan2(h - eye, dist) * 57.29578f;
                if (angle > bestAngle) {
                    bestAngle = angle;
                    bestDist = dist;
                }
            }
            if (bestDist > 2000.0f) {
                ++openAzimuths;
            }
            if (bestAngle > 2.0f && bestDist > 3000.0f) {
                landmark = true;
            }
        }
        // Objective layer 1: an alpine summit (>500 m over sea) within
        // 8 km. Layer 2: a marked hill (>120 m over its 1 km ring)
        // within 4 km.
        f32 dSummit = 1.0e9f;
        for (f32 sz = -8000.0f; sz <= 8000.0f; sz += 250.0f) {
            for (f32 sx = -8000.0f; sx <= 8000.0f; sx += 250.0f) {
                if (ha(p.x + sx, p.z + sz) > sea + 500.0f) {
                    dSummit =
                        glm::min(dSummit, std::hypot(sx, sz));
                }
            }
        }
        f32 dHill = 1.0e9f;
        for (f32 sz = -4000.0f; sz <= 4000.0f; sz += 250.0f) {
            for (f32 sx = -4000.0f; sx <= 4000.0f; sx += 250.0f) {
                const f32 top = ha(p.x + sx, p.z + sz);
                if (top < sea + 60.0f) {
                    continue;
                }
                // A MARKED hill dominates its 500 m disc (not a ravine
                // rim) and stands 120 m over its 1 km ring.
                f32 ring = 0.0f;
                bool localMax = true;
                for (u32 k = 0; k < 8; ++k) {
                    const f32 angle =
                        static_cast<f32>(k) * (6.2831853f / 8.0f);
                    const f32 kx = std::cos(angle);
                    const f32 kz = std::sin(angle);
                    ring += ha(p.x + sx + kx * 1000.0f,
                               p.z + sz + kz * 1000.0f);
                    if (ha(p.x + sx + kx * 500.0f,
                           p.z + sz + kz * 500.0f) > top) {
                        localMax = false;
                        break;
                    }
                }
                if (localMax && top - ring / 8.0f > 120.0f) {
                    dHill = glm::min(dHill, std::hypot(sx, sz));
                }
            }
        }
        const bool open = openAzimuths >= 30;
        const bool summit = dSummit <= 8000.0f;
        const bool hill = dHill <= 4000.0f;
        openOk += open;
        landmarkOk += landmark;
        summitOk += summit;
        hillOk += hill;
        MESSAGE("point (", p.x, ", ", p.z, ") h=", p.h, ": open az ",
                openAzimuths, "/72",
                std::string(landmark ? "" : "  NO-LANDMARK"), "  summit ",
                dSummit < 1.0e9f ? dSummit / 1000.0f : -1.0f,
                " km  hill ",
                dHill < 1.0e9f ? dHill / 1000.0f : -1.0f, " km");
    }
    const u32 n = glm::max<u32>(1, static_cast<u32>(points.size()));
    MESSAGE("summary: open>=30az ", openOk, "/", n, "  landmark>2°@3km ",
            landmarkOk, "/", n, "  alpine summit<=8km ", summitOk, "/",
            n, "  marked hill<=4km ", hillOk, "/", n);
    CHECK(true);
}

// 2-D family census on the spawn tile: every 250 m window of the baked
// region classified socle/versant/drame with its relief — the fair
// instrument for the 40/35/25 budget (a straight transect over- or
// under-samples one family), and the proof that calm ground is calm.
//   meadows-tests '-tc=family census*' -ns
TEST_CASE("family census diagnostic" * doctest::skip()) {
    TileBakeParams params;
    params.worldSeed = 1337;
    ProceduralControlParams controlParams = params.controls;
    controlParams.seed = params.worldSeed;
    const ProceduralControls controls { controlParams };
    const TileBakeResult baked = bakeTile(params, 2, 0);
    const auto& r = baked.region;
    constexpr f32 kWindow = 250.0f;
    const u32 stride = static_cast<u32>(kWindow / r.texelSize);
    u32 socle = 0, versant = 0, drame = 0, wet = 0, plateau = 0;
    vector<f32> socleRelief, versantRelief, allRelief;
    for (u32 wz = 0; wz + stride < r.height; wz += stride) {
        for (u32 wx = 0; wx + stride < r.width; wx += stride) {
            f32 minH = 1.0e9f, maxH = -1.0e9f;
            vector<f32> slopes;
            for (u32 row = wz; row < wz + stride; row += 2) {
                for (u32 col = wx; col < wx + stride; col += 2) {
                    const size_t i =
                        static_cast<size_t>(row) * r.width + col;
                    const f32 h = r.heights[i];
                    minH = glm::min(minH, h);
                    maxH = glm::max(maxH, h);
                    if (col + 2 < wx + stride) {
                        slopes.push_back(
                            std::abs(r.heights[i + 2] - h) /
                            (2.0f * r.texelSize));
                    }
                }
            }
            if (maxH < params.macro.seaLevel + 0.5f) {
                ++wet;
                continue;
            }
            const f32 relief = maxH - minH;
            allRelief.push_back(relief);
            std::sort(slopes.begin(), slopes.end());
            const f32 medianSlope = slopes[slopes.size() / 2];
            if (medianSlope > 0.5774f) {
                ++drame;
            } else if (medianSlope < 0.1763f && relief < 15.0f) {
                ++socle;
                socleRelief.push_back(relief);
                if (controls
                        .at(r.originX +
                                (static_cast<f32>(wx) + stride * 0.5f) *
                                    r.texelSize,
                            r.originZ +
                                (static_cast<f32>(wz) + stride * 0.5f) *
                                    r.texelSize)
                        .plateau > 80.0f) {
                    ++plateau;
                }
            } else {
                ++versant;
                versantRelief.push_back(relief);
            }
        }
    }
    const auto median = [](vector<f32>& v) {
        if (v.empty()) {
            return 0.0f;
        }
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    const f32 land = static_cast<f32>(socle + versant + drame);
    MESSAGE("spawn tile 250m windows: socle ", 100.0f * socle / land,
            "% (dont plateau ", 100.0f * plateau / land, "%), versant ",
            100.0f * versant / land, "%, drame ",
            100.0f * drame / land, "%  (", wet,
            " wet)  target 40/35/25, land only");
    MESSAGE("median relief: all ", median(allRelief), " m, socle ",
            median(socleRelief), " m, versant ", median(versantRelief),
            " m");
    CHECK(land > 0.0f);
}

// Calm-family coverage on a REAL tile: how much of the spawn tile's dry
// ground the stage-1 `calm` field claims (control level + valley-floor
// fusion) — the input B2 damps erosion with. Watch it against the ~40%
// socle budget.
//   meadows-tests '-tc=calm coverage*' -ns
TEST_CASE("calm coverage diagnostic" * doctest::skip()) {
    TileBakeParams params;
    params.worldSeed = 1337;
    const TileStage1 s1 = bakeTileStage1(params, 2, 0);
    u64 dry = 0, calm06 = 0, calm03 = 0, fromControl = 0;
    ProceduralControlParams controlParams = params.controls;
    controlParams.seed = params.worldSeed;
    const ProceduralControls controls { controlParams };
    for (u32 row = 0; row < s1.sim.n; row += 2) {
        for (u32 col = 0; col < s1.sim.n; col += 2) {
            const size_t i = static_cast<size_t>(row) * s1.sim.n + col;
            if (s1.eroded[i] <= params.macro.seaLevel) {
                continue;
            }
            ++dry;
            calm06 += s1.calm[i] > 0.6f;
            calm03 += s1.calm[i] > 0.3f;
            fromControl +=
                controls.at(s1.sim.x(col), s1.sim.z(row)).calm > 0.6f;
        }
    }
    MESSAGE("spawn tile stage-1: dry cells ", dry, "  calm>0.6 ",
            100.0 * calm06 / dry, "%  calm>0.3 ", 100.0 * calm03 / dry,
            "%  control-only calm>0.6 ", 100.0 * fromControl / dry, "%");
    CHECK(dry > 0);
}

// UV health of the scanned rocks (docs/GRASS-REDO.md props): per model,
// the per-triangle texel-stretch distribution (world area vs uv area)
// BEFORE and AFTER decimation — the striped-face hunt (the texture
// not wrapping around the pale boulder).
//   meadows-tests '-tc=rock uv diagnostic' -ns
#include "engine/assets/GltfMesh.hpp"
#include "engine/assets/MeshSimplify.hpp"
TEST_CASE("snow coverage diagnostic" * doctest::skip()) {
    // Snow-line calibration instrument: bakes the spawn tile and its two
    // N-S neighbours, then measures the snow WEIGHT coverage of the land
    // under several (base snow line, biome offsets) configs through the
    // real materialWeightsAt path (blended attributes + wander field
    // included). "full" = weight > 0.5, "touched" = the deposition
    // overlay's band has begun (h > line - 90). Altitude bands locate
    // where the snow lives.
    TileBakeParams params;
    params.worldSeed = 1337;
    ProceduralControlParams controlParams = params.controls;
    controlParams.seed = params.worldSeed;
    const f32 seaLevel = params.macro.seaLevel;

    auto base = std::make_shared<render::TerrainBase>();
    for (const i32 tz : { -1, 0, 1 }) {
        MESSAGE("baking tile (2, ", tz, ")");
        base->regions.push_back(bakeTile(params, 2, tz).region);
    }
    render::TerrainParams tp;
    tp.base = base;
    tp.seed = params.worldSeed;
    auto sandbox = std::make_shared<render::SandboxTerrain>();
    sandbox->controls = controlParams;
    sandbox->macro = params.macro;
    tp.sandbox = sandbox;

    struct Config {
        const char* label;
        f32 snowLine;
        f32 arid;
        f32 alpine;
        f32 tundra;
    };
    // "avant-M4" is the pre-M4 look the calibration targets (~6 % full
    // snow); "adopte" is the shipped landscape.toml config, the others
    // bracket it one notch either way for future retuning.
    const Config configs[] = {
        { "avant-M4", 1100.0f, 400.0f, -300.0f, -650.0f },
        { "adopte  ", 900.0f, 150.0f, -180.0f, -300.0f },
        { "var-950 ", 950.0f, 150.0f, -150.0f, -250.0f },
        { "var-850 ", 850.0f, 200.0f, -200.0f, -350.0f },
    };
    const auto biomeSetFor = [](const Config& c) {
        auto set = std::make_shared<render::BiomeSet>();
        set->table.resize(6);
        set->table[4].rockiness = 0.35f;
        set->table[4].grassPresence = 0.55f;
        set->table[4].snowLineOffset = -60.0f;
        set->table[4].temperature = -0.2f;
        set->table[4].wetness = 0.25f;
        set->table[5].sandiness = 0.35f;
        set->table[5].grassPresence = 0.6f;
        set->table[5].snowLineOffset = 80.0f;
        set->table[5].temperature = 0.35f;
        set->table[5].wetness = 0.15f;
        set->table[1].sandiness = 0.7f;
        set->table[1].grassPresence = 0.25f;
        set->table[1].snowLineOffset = c.arid;
        set->table[1].temperature = 0.6f;
        set->table[1].wetness = 0.1f;
        set->table[2].rockiness = 0.6f;
        set->table[2].grassPresence = 0.6f;
        set->table[2].snowLineOffset = c.alpine;
        set->table[2].temperature = -0.4f;
        set->table[3].rockiness = 0.3f;
        set->table[3].grassPresence = 0.4f;
        set->table[3].snowLineOffset = c.tundra;
        set->table[3].temperature = -0.8f;
        return set;
    };
    constexpr f32 kBandEdges[] = { 300.0f, 600.0f, 900.0f, 1200.0f };
    for (const Config& c : configs) {
        tp.snowLine = c.snowLine;
        tp.biomes = biomeSetFor(c);
        u32 land = 0;
        u32 full = 0;
        u32 touched = 0;
        u32 bandLand[5] = {};
        u32 bandFull[5] = {};
        for (const render::TerrainRegion& region : base->regions) {
            for (f32 z = region.originZ + 200.0f;
                 z < region.originZ + region.spanZ() - 200.0f;
                 z += 32.0f) {
                for (f32 x = region.originX + 200.0f;
                     x < region.originX + region.spanX() - 200.0f;
                     x += 32.0f) {
                    const f32 h = render::terrain::height(tp, x, z);
                    if (h < seaLevel + 0.5f) {
                        continue;
                    }
                    ++land;
                    const Vec3 n = render::terrain::normal(tp, x, z);
                    const auto w = render::terrain::materialWeightsAt(
                        tp, x, z, h, n);
                    const auto fields =
                        render::terrain::regionFieldsAt(tp, x, z);
                    const f32 line = c.snowLine + fields.snowLineOffset;
                    u32 band = 0;
                    while (band < 4 && h >= kBandEdges[band]) {
                        ++band;
                    }
                    ++bandLand[band];
                    if (w.snow > 0.5f) {
                        ++full;
                        ++bandFull[band];
                    }
                    if (h > line - 90.0f) {
                        ++touched;
                    }
                }
            }
        }
        const auto pct = [](u32 num, u32 den) {
            return den ? 100.0f * static_cast<f32>(num) /
                             static_cast<f32>(den)
                       : 0.0f;
        };
        MESSAGE(std::string(c.label), " base ", c.snowLine, " offsets(",
                c.arid, "/", c.alpine, "/", c.tundra, "): full ",
                pct(full, land), "%  touched ", pct(touched, land),
                "%  (", land, " land texels)");
        MESSAGE("   full by band  <300m ", pct(bandFull[0], bandLand[0]),
                "%  300-600 ", pct(bandFull[1], bandLand[1]),
                "%  600-900 ", pct(bandFull[2], bandLand[2]),
                "%  900-1200 ", pct(bandFull[3], bandLand[3]),
                "%  >1200 ", pct(bandFull[4], bandLand[4]), "%");
    }
    CHECK(true);
}

TEST_CASE("biome locator diagnostic" * doctest::skip()) {
    // Where is each biome? Scans the control fields around the spawn
    // (controls only — no bake) and prints, per palette id, the nearest
    // LAND occurrence plus a far alternate, with the analytic height so
    // the console/fly coordinate can be pasted directly (x, y, z).
    TileBakeParams params;
    params.worldSeed = 1337;
    ProceduralControlParams controlParams = params.controls;
    controlParams.seed = params.worldSeed;
    const ProceduralControls controls { controlParams };
    const f32 px = 8196.77f;
    const f32 pz = 230.072f;
    const char* names[] = { "temperate", "arid",      "alpine",
                            "tundra",    "subalpine", "steppe" };
    struct Hit {
        f32 x { 0.0f };
        f32 z { 0.0f };
        f32 d { 1.0e18f };
    };
    Hit nearest[6];
    Hit alternate[6]; // nearest beyond 6 km — a second spot to try
    for (f32 z = pz - 24000.0f; z <= pz + 24000.0f; z += 96.0f) {
        for (f32 x = px - 24000.0f; x <= px + 24000.0f; x += 96.0f) {
            const ControlSample s = controls.at(x, z);
            if (s.sea || s.biome >= 6) {
                continue;
            }
            const f32 dx = x - px;
            const f32 dz = z - pz;
            const f32 d = dx * dx + dz * dz;
            if (d < nearest[s.biome].d) {
                nearest[s.biome] = { x, z, d };
            }
            if (d > 6000.0f * 6000.0f && d < alternate[s.biome].d) {
                alternate[s.biome] = { x, z, d };
            }
        }
    }
    // Spawn-probe mirror (LandscapeScene setSandbox — keep the criteria
    // in sync): where the game will actually start, temperate decree
    // included.
    {
        f32 sx = 2600.0f;
        f32 sz = 0.0f;
        f32 sy = 0.0f;
        bool found = false;
        for (f32 radius = 2600.0f; radius <= 24000.0f && !found;
             radius += 700.0f) {
            for (u32 step = 0; step < 16 && !found; ++step) {
                const f32 angle =
                    radius * 0.0137f + static_cast<f32>(step) * 0.3927f;
                const f32 x = std::cos(angle) * radius;
                const f32 z = std::sin(angle) * radius;
                const f32 h = macroHeightAnalytic(controls, params.macro,
                                                  x, z);
                if (h > params.macro.seaLevel + 8.0f && h < 95.0f &&
                    controls.at(x, z).biome == 0) {
                    sx = x;
                    sz = z;
                    sy = h;
                    found = true;
                }
            }
        }
        MESSAGE("spawn probe mirror: (", static_cast<i32>(sx), ", ",
                static_cast<i32>(sy + 2.0f), ", ", static_cast<i32>(sz),
                ")  found=", found);
    }
    // Patch geometry at the nearest steppe hit: how big is the zone the
    // player is sent to, and how strong does the blended sandiness (the
    // shader's aridity signal) actually get there?
    if (nearest[5].d < 1.0e18f) {
        const f32 cx = nearest[5].x;
        const f32 cz = nearest[5].z;
        u32 steppe = 0;
        u32 arid = 0;
        u32 total = 0;
        f32 maxSand = 0.0f;
        for (f32 z = cz - 750.0f; z <= cz + 750.0f; z += 24.0f) {
            for (f32 x = cx - 750.0f; x <= cx + 750.0f; x += 24.0f) {
                const ControlSample s = controls.at(x, z);
                ++total;
                steppe += s.biome == 5 ? 1 : 0;
                arid += s.biome == 1 ? 1 : 0;
                // The runtime cross-blend, approximated at control level.
                f32 sand = 0.0f;
                for (const Vec2 o : { Vec2 { 0, 0 }, Vec2 { 32, 0 },
                                      Vec2 { -32, 0 }, Vec2 { 0, 32 },
                                      Vec2 { 0, -32 } }) {
                    const u8 id = controls.at(x + o.x, z + o.y).biome;
                    const f32 w = (o.x == 0.0f && o.y == 0.0f) ? 2.0f
                                                               : 1.0f;
                    sand += w * (id == 1 ? 0.7f : id == 5 ? 0.35f : 0.0f);
                }
                maxSand = glm::max(maxSand, sand / 6.0f);
            }
        }
        MESSAGE("steppe patch @nearest: steppe ",
                100.0f * static_cast<f32>(steppe) /
                    static_cast<f32>(total),
                "%  arid ",
                100.0f * static_cast<f32>(arid) / static_cast<f32>(total),
                "% of the 1.5 km box, max blended sandiness ", maxSand);
    }
    for (u32 b = 0; b < 6; ++b) {
        const auto report = [&](const char* tag, const Hit& hit) {
            const std::string label =
                std::string(names[b]) + " " + tag;
            if (hit.d >= 1.0e18f) {
                MESSAGE(label, ": none within 24 km");
                return;
            }
            const f32 y = macroHeightAnalytic(controls, params.macro,
                                              hit.x, hit.z);
            MESSAGE(label, ": (", static_cast<i32>(hit.x), ", ",
                    static_cast<i32>(y + 40.0f), ", ",
                    static_cast<i32>(hit.z), ")  a ",
                    static_cast<i32>(std::sqrt(hit.d)), " m du spawn");
        };
        report("nearest", nearest[b]);
        report("alt>6km", alternate[b]);
    }
    CHECK(true);
}

TEST_CASE("lake census diagnostic" * doctest::skip()) {
    // Lake size distribution over the spawn neighbourhood: where does
    // the puddle tail end and the real lakes begin? Areas from the
    // flooded masks (never the bbox), max depth from level - min ground.
    TileBakeParams params;
    params.worldSeed = 1337;
    struct Bucket {
        f32 maxArea; // m²
        const char* label;
        u32 count { 0 };
        f32 deepest { 0.0f };
    };
    Bucket buckets[] = {
        { 1000.0f, "<0.1ha  " },
        { 5000.0f, "0.1-0.5 " },
        { 20000.0f, "0.5-2ha " },
        { 1.0e18f, ">2ha    " },
    };
    u32 total = 0;
    u32 dug = 0;
    u32 tierCount[3] = {};
    u32 fordCount = 0;
    for (const auto [tx, tz] : { std::pair { 2, -1 }, { 2, 0 }, { 2, 1 },
                                 { 1, 0 }, { 3, 0 } }) {
        MESSAGE("baking tile (", tx, ", ", tz, ")");
        const TileBakeResult baked = bakeTile(params, tx, tz);
        for (const River& river : baked.rivers) {
            ++tierCount[glm::min<u32>(river.tier, 2)];
            fordCount += static_cast<u32>(river.fords.size());
            if (river.tier == 2 && !river.points.empty()) {
                const RiverPoint& mid =
                    river.points[river.points.size() / 2];
                MESSAGE("  fleuve run: mid (", static_cast<i32>(mid.x),
                        ", ", static_cast<i32>(mid.surface), ", ",
                        static_cast<i32>(mid.z), "), hw ",
                        mid.halfWidth, ", ", river.points.size(),
                        " pts");
            }
        }
        auto base = std::make_shared<render::TerrainBase>();
        base->regions.push_back(baked.region);
        render::TerrainParams tp;
        tp.base = base;
        for (const Lake& lake : baked.lakes) {
            if (lake.dug) {
                ++dug; // placed ponds: design features, never filtered
                continue;
            }
            u32 wet = 0;
            f32 depth = 0.0f;
            for (u32 mz = 0; mz < lake.maskHeight; ++mz) {
                for (u32 mx = 0; mx < lake.maskWidth; ++mx) {
                    if (!lake.mask[static_cast<size_t>(mz) *
                                       lake.maskWidth +
                                   mx]) {
                        continue;
                    }
                    ++wet;
                    const f32 x = lake.minX + (static_cast<f32>(mx) +
                                               0.5f) *
                                                  lake.maskTexel;
                    const f32 z = lake.minZ + (static_cast<f32>(mz) +
                                               0.5f) *
                                                  lake.maskTexel;
                    depth = glm::max(
                        depth,
                        lake.level - render::terrain::height(tp, x, z));
                }
            }
            const f32 area =
                static_cast<f32>(wet) * lake.maskTexel * lake.maskTexel;
            ++total;
            for (Bucket& b : buckets) {
                if (area <= b.maxArea) {
                    ++b.count;
                    b.deepest = glm::max(b.deepest, depth);
                    break;
                }
            }
        }
    }
    MESSAGE("natural lakes over 5 tiles: ", total, "  (+ ", dug,
            " placed ponds, never filtered)");
    MESSAGE("river runs by tier: ruisseau ", tierCount[0], "  riviere ",
            tierCount[1], "  fleuve ", tierCount[2], "  | fords ",
            fordCount);
    for (const Bucket& b : buckets) {
        MESSAGE("  ", std::string(b.label), ": ", b.count,
                "  (deepest ", b.deepest, " m)");
    }
    CHECK(true);
}

TEST_CASE("river wetness diagnostic" * doctest::skip()) {
    // The ground truth of "l'eau est continue dans les creusements" :
    // walks every published river run's centerline at 2 m and probes the
    // baked terrain against the ribbon surface — DRY means the water
    // sheet is clipped under the ground there. Also lists the run ends
    // (each end is a place the ribbon dissolves — too many of them and
    // the course reads as broken puddles).
    TileBakeParams params;
    params.worldSeed = 1337;
    for (const auto [tx, tz] : { std::pair { 2, 0 }, { 2, 1 } }) {
        MESSAGE("baking tile (", tx, ", ", tz, ")");
        const TileBakeResult baked = bakeTile(params, tx, tz);
        auto base = std::make_shared<render::TerrainBase>();
        base->regions.push_back(baked.region);
        render::TerrainParams tp;
        tp.base = base;
        u32 samples = 0;
        u32 dry = 0;
        f32 worstDryRun = 0.0f;
        f32 totalLen = 0.0f;
        u32 runs = 0;
        u32 shortRuns = 0; // < 100 m: crop confetti, all ends dissolving
        f32 worstFlat = 0.0f; // longest LEVEL surface stretch, tier 2 —
                              // the "fleuve reads as a lake" measure
        for (const River& river : baked.rivers) {
            if (river.points.size() < 2) {
                continue;
            }
            ++runs;
            if (river.tier == 2) {
                f32 flat = 0.0f;
                for (size_t s = 0; s + 1 < river.points.size(); ++s) {
                    const f32 len = std::hypot(
                        river.points[s + 1].x - river.points[s].x,
                        river.points[s + 1].z - river.points[s].z);
                    if (river.points[s].surface -
                            river.points[s + 1].surface <
                        0.01f) {
                        flat += len;
                        worstFlat = glm::max(worstFlat, flat);
                    } else {
                        flat = 0.0f;
                    }
                }
            }
            f32 runLen = 0.0f;
            f32 dryStretch = 0.0f;
            for (size_t s = 0; s + 1 < river.points.size(); ++s) {
                const RiverPoint& a = river.points[s];
                const RiverPoint& b = river.points[s + 1];
                const f32 len = std::hypot(b.x - a.x, b.z - a.z);
                runLen += len;
                const i32 n =
                    glm::max(static_cast<i32>(len / 2.0f), 1);
                for (i32 i = 0; i < n; ++i) {
                    const f32 t =
                        static_cast<f32>(i) / static_cast<f32>(n);
                    const f32 x = glm::mix(a.x, b.x, t);
                    const f32 z = glm::mix(a.z, b.z, t);
                    const f32 surface =
                        glm::mix(a.surface, b.surface, t);
                    const f32 h = render::terrain::height(tp, x, z);
                    ++samples;
                    if (h > surface - 0.05f) {
                        ++dry;
                        dryStretch += 2.0f;
                        if (dryStretch > worstDryRun) {
                            worstDryRun = dryStretch;
                            MESSAGE("    dry at (",
                                    static_cast<i32>(x), ", ",
                                    static_cast<i32>(z), "): terrain ",
                                    h, " vs surface ", surface,
                                    " (hw ",
                                    glm::mix(a.halfWidth, b.halfWidth,
                                             t),
                                    ", stretch ", dryStretch, " m)");
                        }
                    } else {
                        dryStretch = 0.0f;
                    }
                }
            }
            totalLen += runLen;
            shortRuns += runLen < 100.0f ? 1 : 0;
        }
        MESSAGE("  ", runs, " runs, ", totalLen / 1000.0f,
                " km total, ", shortRuns, " runs < 100 m");
        MESSAGE("  centerline dry: ",
                100.0f * static_cast<f32>(dry) /
                    static_cast<f32>(glm::max(samples, 1u)),
                "%  worst continuous dry stretch ", worstDryRun, " m");
        MESSAGE("  longest LEVEL fleuve surface: ", worstFlat, " m");
    }
    CHECK(true);
}

TEST_CASE("fleuve locator diagnostic" * doctest::skip()) {
    // Where are the fleuves? Master-network courses around the spawn
    // (no bake needed — the promotion follows these very polylines).
    // Prints, per course, its nearest point to the spawn and its
    // biggest-area node, as pasteable (x, y, z).
    TileBakeParams params;
    params.worldSeed = 1337;
    ProceduralControlParams controlParams = params.controls;
    controlParams.seed = params.worldSeed;
    const ProceduralControls controls { controlParams };
    MasterNetworkParams network = params.network;
    network.seaLevel = params.macro.seaLevel;
    const f32 px = 8196.77f;
    const f32 pz = 230.072f;
    const auto rivers = masterRiversNear(
        controls, params.macro, network, px - 30000.0f, pz - 30000.0f,
        px + 30000.0f, pz + 30000.0f);
    MESSAGE("master courses in +/-30 km: ", rivers.size());
    struct Entry {
        f32 dist;
        f32 nx, nz;   // nearest node to spawn
        f32 bx, bz;   // biggest-area node (the wide stretch)
        f32 area;
        f32 length;
    };
    vector<Entry> entries;
    for (const auto& river : rivers) {
        if (river.nodes.size() < 2) {
            continue;
        }
        Entry e { 1.0e30f, 0, 0, 0, 0, 0.0f, 0.0f };
        for (size_t i = 0; i < river.nodes.size(); ++i) {
            const MasterNode& node = river.nodes[i];
            const f32 dx = node.x - px;
            const f32 dz = node.z - pz;
            const f32 d = std::sqrt(dx * dx + dz * dz);
            if (d < e.dist) {
                e.dist = d;
                e.nx = node.x;
                e.nz = node.z;
            }
            if (node.area > e.area) {
                e.area = node.area;
                e.bx = node.x;
                e.bz = node.z;
            }
            if (i > 0) {
                e.length += std::hypot(node.x - river.nodes[i - 1].x,
                                       node.z - river.nodes[i - 1].z);
            }
        }
        entries.push_back(e);
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) {
                  return a.dist < b.dist;
              });
    for (size_t i = 0; i < glm::min<size_t>(entries.size(), 6); ++i) {
        const Entry& e = entries[i];
        const f32 yn = macroHeightAnalytic(controls, params.macro, e.nx,
                                           e.nz);
        const f32 yb = macroHeightAnalytic(controls, params.macro, e.bx,
                                           e.bz);
        MESSAGE("fleuve ", i, ": ", static_cast<i32>(e.length / 1000),
                " km, nearest (", static_cast<i32>(e.nx), ", ",
                static_cast<i32>(yn + 30.0f), ", ",
                static_cast<i32>(e.nz), ") a ",
                static_cast<i32>(e.dist), " m du spawn | large a (",
                static_cast<i32>(e.bx), ", ",
                static_cast<i32>(yb + 30.0f), ", ",
                static_cast<i32>(e.bz), ")");
    }
    CHECK(true);
}

TEST_CASE("fleuve continuity diagnostic" * doctest::skip()) {
    // Repro of the vanished-fleuve report: the flagged spot (8192, 6656)
    // sits ON the tile border (1,1)|(2,1). Bake both owners and list
    // every river run passing within 800 m — do the two sides agree on
    // the course's existence, tier and width where they meet?
    TileBakeParams params;
    params.worldSeed = 1337;
    struct Spot {
        i32 tx, tz;
        f32 fx, fz;
    };
    const Spot spots[] = {
        { 1, 1, 8192.0f, 6656.0f }, // the vanished-fleuve report
        { 2, 1, 8192.0f, 6656.0f },
        { 2, 4, 10240.0f, 17536.0f }, // the far mouth stretch
    };
    for (const auto& [tx, tz, fx, fz] : spots) {
        MESSAGE("baking tile (", tx, ", ", tz, ")");
        const TileBakeResult baked = bakeTile(params, tx, tz);
        u32 shown = 0;
        for (const River& river : baked.rivers) {
            f32 best = 1.0e30f;
            for (const RiverPoint& pt : river.points) {
                const f32 dx = pt.x - fx;
                const f32 dz = pt.z - fz;
                best = glm::min(best, dx * dx + dz * dz);
            }
            if (best > 800.0f * 800.0f) {
                continue;
            }
            const RiverPoint& head = river.points.front();
            const RiverPoint& tail = river.points.back();
            MESSAGE("  run tier ", static_cast<u32>(river.tier), " (",
                    river.points.size(), " pts, hw ",
                    head.halfWidth, " -> ", tail.halfWidth,
                    "): head (", static_cast<i32>(head.x), ", ",
                    static_cast<i32>(head.z), ") tail (",
                    static_cast<i32>(tail.x), ", ",
                    static_cast<i32>(tail.z), "), a ",
                    static_cast<i32>(std::sqrt(best)), " m du point");
            ++shown;
        }
        MESSAGE("  -> ", shown, " run(s) near the spot");
    }
    CHECK(true);
}

TEST_CASE("analytic sea mismatch diagnostic" * doctest::skip()) {
    // The far mesh beyond baked tiles renders macroHeightAnalytic; over
    // the ocean its shore distance is a continentalness PROXY, so any
    // carrier bump barely above the sea threshold can invent land the
    // bake never confirms — phantom islands on the horizon. Bakes the
    // oceanic tile (3, 0) and counts, over TRUE sea texels (baked
    // < seaLevel - 2), how often and how high the analytic pokes above
    // the water.
    TileBakeParams params;
    params.worldSeed = 1337;
    ProceduralControlParams controlParams = params.controls;
    controlParams.seed = params.worldSeed;
    const ProceduralControls controls { controlParams };
    const f32 seaLevel = params.macro.seaLevel;
    MESSAGE("baking tile (3, 0)");
    auto base = std::make_shared<render::TerrainBase>();
    base->regions.push_back(bakeTile(params, 3, 0).region);
    const render::TerrainRegion& region = base->regions.front();
    render::TerrainParams tp;
    tp.base = base;
    u32 seaTexels = 0;
    u32 phantom = 0;
    u32 phantomBig = 0;
    f32 worst = 0.0f;
    f32 worstX = 0.0f;
    f32 worstZ = 0.0f;
    for (f32 z = region.originZ + 200.0f;
         z < region.originZ + region.spanZ() - 200.0f; z += 24.0f) {
        for (f32 x = region.originX + 200.0f;
             x < region.originX + region.spanX() - 200.0f; x += 24.0f) {
            const f32 hb = render::terrain::height(tp, x, z);
            if (hb >= seaLevel - 2.0f) {
                continue;
            }
            ++seaTexels;
            const f32 ha =
                macroHeightAnalytic(controls, params.macro, x, z);
            const f32 above = ha - (seaLevel + 1.0f);
            if (above > 0.0f) {
                ++phantom;
                phantomBig += above > 8.0f ? 1 : 0;
                if (above > worst) {
                    worst = above;
                    worstX = x;
                    worstZ = z;
                }
            }
        }
    }
    const f32 pct = seaTexels > 0 ? 100.0f * static_cast<f32>(phantom) /
                                        static_cast<f32>(seaTexels)
                                  : 0.0f;
    const f32 pctBig = seaTexels > 0
        ? 100.0f * static_cast<f32>(phantomBig) /
              static_cast<f32>(seaTexels)
        : 0.0f;
    MESSAGE("sea texels ", seaTexels, ": analytic above water ", pct,
            "%  (>8 m: ", pctBig, "%)  worst +", worst, " m at (",
            static_cast<i32>(worstX), ", ", static_cast<i32>(worstZ),
            ")");
    CHECK(true);
}

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
